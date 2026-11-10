"""qt_backend.py - runs the launcher's Python side for the Qt window (launcher-qt/).

The launcher's logic stays in uncharted_launcher.pyw. This process builds that launcher with its Tk window hidden
and lets the Qt window show and use it:

  stdin   one JSON request per line from the Qt window
  stdout  one JSON reply / event per line (everything else the launcher prints goes to stderr)

Requests ({"op": ...}):
  model   {"tab": name}       the window (game cards, tabs, status, PLAY) and that tab's page as a tree of
                              sections, rows and controls; "same": true when nothing changed since the last reply
  act     {"id", "kind", "value"}  what a click on a control does in the Tk window: toggle / choice / dropdown /
                              chip -> set(value, notify), button -> action(), entry -> new text, click / dclick ->
                              the widget's own <Button-1> / <Double-Button-1> binding
  game    {"key"}             select a game;  "play": {"key"} plays it (double-click on a card)
  tab     {"name"}            show a tab
  play                        PLAY / STOP
  update                      UPDATE (install the newer release)
  answer  {"dialog", "value"} the reply to a dialog event
  close                       close the launcher (as the window's close button)
Events ({"ev": ...}):
  dialog  {"dialog", "kind", "title", "message", ...}: a message box or file dialog the launcher asked for; the
          launcher waits until the Qt window answers.
  closed  the launcher ended.

Nothing here decides anything: every value comes from the Tk widgets, every action runs their own code.
"""
import hashlib
import importlib.util
import json
import os
import queue
import sys
import threading
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

PROTO = os.fdopen(os.dup(sys.stdout.fileno()), "w", encoding="utf-8", newline="\n")
sys.stdout = sys.stderr   # the launcher's own prints must not reach the protocol stream
_out_lock = threading.Lock()


def send(message):
    with _out_lock:
        PROTO.write(json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n")
        PROTO.flush()


spec = importlib.util.spec_from_file_location("uncharted_launcher", HERE / "uncharted_launcher.pyw")
ul = importlib.util.module_from_spec(spec)
sys.modules["uncharted_launcher"] = ul
spec.loader.exec_module(ul)
tk = ul.tk

requests = queue.Queue()


def reader():
    for line in sys.stdin:
        line = line.strip()
        if line:
            try:
                requests.put(json.loads(line))
            except ValueError:
                pass
    requests.put({"op": "close"})


# -- dialogs: the launcher's message boxes and file dialogs are shown by the Qt window ------------------------
_dialog_ids = iter(range(1, 1 << 30))
_held = []


def ask(kind, **fields):
    dialog = next(_dialog_ids)
    send(dict(ev="dialog", dialog=dialog, kind=kind, **{k: ("" if v is None else str(v)) for k, v in fields.items()}))
    while True:
        message = requests.get()
        if message.get("op") == "answer" and message.get("dialog") == dialog:
            return message.get("value")
        if message.get("op") == "close":
            requests.put(message)
            return None
        _held.append(message)


def _box(kind):
    def show(title=None, message=None, **options):
        value = ask(kind, title=title, message=message, detail=options.get("detail"))
        if kind in ("info", "warning", "error"):
            return "ok"
        if kind == "yesnocancel":
            return None if value is None else bool(value)
        return bool(value)
    return show


def _file(kind):
    def show(**options):
        types = options.get("filetypes") or ()
        value = ask(kind, title=options.get("title"), initialdir=options.get("initialdir"),
                    initialfile=options.get("initialfile"),
                    filetypes=";;".join("%s (%s)" % (name, pattern if isinstance(pattern, str) else " ".join(pattern))
                                        for name, pattern in types))
        return value or ""
    return show


from tkinter import filedialog, messagebox  # noqa: E402

for name, kind in (("showinfo", "info"), ("showwarning", "warning"), ("showerror", "error"),
                   ("askyesno", "yesno"), ("askokcancel", "okcancel"), ("askyesnocancel", "yesnocancel"),
                   ("askretrycancel", "retrycancel"), ("askquestion", "yesno")):
    fn = _box(kind)
    if name == "askquestion":
        fn = (lambda f: lambda *a, **k: "yes" if f(*a, **k) else "no")(fn)
    setattr(messagebox, name, fn)
    setattr(ul.messagebox, name, fn)
filedialog.askdirectory = _file("directory")
filedialog.askopenfilename = _file("open")
filedialog.asksaveasfilename = _file("save")


# -- the model -------------------------------------------------------------------------------------------------
def text_of(widget):
    try:
        return widget.cget("text")
    except tk.TclError:
        return ""


def font_info(widget):
    try:
        font = ul.tkfont.Font(font=widget.cget("font"))
        actual = font.actual()
        return {"size": abs(actual.get("size", 10)), "bold": actual.get("weight") == "bold" or
                "Semibold" in str(actual.get("family", "")), "family": actual.get("family", "")}
    except tk.TclError:
        return {"size": 10, "bold": False, "family": ""}


def color(widget, key):
    try:
        return str(widget.cget(key))
    except tk.TclError:
        return ""


def shown(widget):
    try:
        return bool(widget.winfo_manager())
    except tk.TclError:
        return False


def ordered_children(widget):
    """Children in layout order: packed ones in pack order, gridded ones by row and column."""
    try:
        packed = widget.pack_slaves()
    except tk.TclError:
        packed = []
    try:
        gridded = widget.grid_slaves()
    except tk.TclError:
        gridded = []
    if gridded:
        def key(w):
            info = w.grid_info()
            return int(info.get("row", 0)), int(info.get("column", 0))
        gridded = sorted(gridded, key=key)
    return packed, gridded


def has_binding(widget, sequence):
    try:
        return bool(widget.bind(sequence))
    except tk.TclError:
        return False


def node(widget):
    """One widget as a dict for the Qt window (None: not shown, or nothing to show)."""
    w = widget
    wid = str(w)
    if getattr(w, "is_sep", False):
        return {"t": "sep"}
    if getattr(w, "card_head", False):
        return None
    if getattr(w, "qt_group", None):   # a group the Qt window can fold away (RPCN's account part)
        items = children(w)
        return {"t": "group", "id": wid, "title": w.qt_group, "items": items} if items else None
    if isinstance(w, ul.Card):
        items = children(w.inner)
        if not items:
            return None
        return {"t": "section", "id": wid, "title": w.title, "note": w.note, "items": items,
                "column": getattr(w, "column", "")}
    if getattr(w, "is_row", False) and hasattr(w, "control"):
        hint = w.hint_label
        control = node(w.control) if shown(w.control) else None
        extra = [n for n in (node(c) for c in w.pack_slaves() if c is not w.control and c.winfo_class() != "Frame")
                 if n]
        return {"t": "row", "id": wid, "title": text_of(w.title_label),
                "hint": text_of(hint) if hint is not None and shown(hint) else "", "control": control, "extra": extra}
    if isinstance(w, ul.Toggle):
        return {"t": "toggle", "id": wid, "v": bool(w.value)}
    if isinstance(w, ul.Segmented):
        return {"t": "choice", "id": wid, "v": w.value, "en": bool(w.enabled),
                "opts": [[str(o), w.labels[o]] for o in w.visible]}
    if isinstance(w, ul.Dropdown):
        return {"t": "dropdown", "id": wid, "v": w.value, "label": w.label,
                "opts": [[str(o), str(label)] for o, label in w.options]}
    if isinstance(w, ul.Chip):
        return {"t": "chip", "id": wid, "v": bool(w.value), "text": w.label}
    if isinstance(w, ul.Pill):
        return {"t": "button", "id": wid, "text": w.text, "kind": w.kind}
    if isinstance(w, (ul.TabPill, ul.ScrollBar)):
        return None
    cls = w.winfo_class()
    if cls == "Label":
        text = text_of(w)
        image = image_file(w)
        click = has_binding(w, "<Button-1>")
        if not text and not image and not click:
            return None
        f = font_info(w)
        return {"t": "label", "id": wid, "text": text, "fg": color(w, "fg"), "bg": color(w, "bg"),
                "size": f["size"], "bold": f["bold"], "serif": "Georgia" in f["family"], "image": image,
                "click": click, "rclick": has_binding(w, "<Button-3>"), "mono": "Consolas" in f["family"],
                "slot": click and color(w, "bg") in (ul.PANEL, ul.PANEL_HI, ul.FIELD, ul.FIELD_HI)}
    if cls == "Entry":
        return {"t": "entry", "id": wid, "v": w.get(), "secret": bool(color(w, "show"))}
    if cls == "Text":
        return {"t": "log", "id": wid, "text": w.get("1.0", "end-1c")[-12000:]}
    if cls == "Listbox":
        try:
            selection = list(w.curselection())
        except tk.TclError:
            selection = []
        return {"t": "list", "id": wid, "items": list(w.get(0, "end")), "sel": selection}
    if cls == "Canvas":
        return canvas_node(w)
    if cls in ("Frame", "Labelframe", "Toplevel"):
        packed, gridded = ordered_children(w)
        if gridded and not packed:
            rows = {}
            for c in gridded:
                n = node(c) if shown(c) else None
                if n:
                    rows.setdefault(int(c.grid_info().get("row", 0)), []).append(n)
            items = [{"t": "box", "dir": "h", "items": r} for _, r in sorted(rows.items())]
            if not items:
                return None
            return {"t": "box", "dir": "v", "id": wid, "items": items, "bg": color(w, "bg")}
        items = []
        horizontal = any(c.pack_info().get("side") in ("left", "right") for c in packed)
        if horizontal:   # as Tk shows them: left-packed in order, then right-packed from the far end
            packed = ([c for c in packed if c.pack_info().get("side") != "right"] +
                      [c for c in packed if c.pack_info().get("side") == "right"][::-1])
        for c in packed:
            n = node(c)
            if n:
                n["side"] = c.pack_info().get("side", "top")
                n["fill"] = c.pack_info().get("fill", "none")
                n["expand"] = str(c.pack_info().get("expand", "0")) in ("1", "true")
                items.append(n)
        if not items:
            return None
        if horizontal:   # a text box followed by a small button (Add friend's ADD): the button goes inside the box
            def entry_of(n):   # a text box, or a frame holding just one (the RPCN module's framed text boxes)
                if n.get("t") == "entry":
                    return n
                if n.get("t") == "box" and len(n.get("items", [])) == 1 and n["items"][0].get("t") == "entry":
                    return n["items"][0]
                return None
            merged = []
            for n in items:
                box = entry_of(merged[-1]) if merged else None
                if n.get("t") == "button" and box is not None and "button" not in box:
                    box = dict(box, button=n, fill="x", side=merged[-1].get("side", "left"))
                    merged[-1] = box
                else:
                    merged.append(n)
            items = merged
            # "Add friend" + its box: the name above, the box across the whole width (as the other fields)
            if (len(items) == 2 and items[0].get("t") == "label" and items[1].get("t") == "entry"
                    and "button" in items[1]):
                items[0] = dict(items[0], side="top")
                items[1] = dict(items[1], side="top", wide=True)
                return {"t": "box", "dir": "v", "id": wid, "items": items, "bg": color(w, "bg")}
        return {"t": "box", "dir": "h" if horizontal else "v", "id": wid, "items": items, "bg": color(w, "bg")}
    return None


_images = {}


def image_file(w):
    """A label's image (a controller button icon) as a PNG file the Qt window can show."""
    try:
        name = str(w.cget("image"))
    except tk.TclError:
        return ""
    if not name:
        return ""
    path = _images.get(name)
    if path is None:
        folder = ul.USER_DIR / "qt-images"
        folder.mkdir(parents=True, exist_ok=True)
        path = str(folder / ("img%d.png" % len(_images)))
        try:
            w.tk.call(name, "write", path, "-format", "png")
        except tk.TclError:
            path = ""
        _images[name] = path
    return path


_app = None   # the launcher (main)


def canvas_node(w):
    """A canvas: a progress bar (rectangles), or a clickable card with text (Quick Setup presets)."""
    wid = str(w)
    for bar, progress in (("u3mp_bar", "u3mp_progress"), ("u2mp_bar", "u2mp_progress")):
        if _app is not None and w is getattr(_app, bar, None):   # the import's bar: the launcher's own numbers
            n, total = tuple(getattr(_app, progress, (0, 0)))[:2]
            return {"t": "progress", "id": wid, "v": max(0.0, min(1.0, n / total)) if total else 0.0}
    try:
        width = max(1, int(w.winfo_width() if w.winfo_width() > 1 else w.winfo_fpixels(w.cget("width"))))
        items = w.find_all()
    except tk.TclError:
        return None
    windows = [w.nametowidget(w.itemcget(item, "window")) for item in items
               if w.type(item) == "window" and w.itemcget(item, "window")]
    if windows:   # a scrolling list (the Controls bindings): what it holds
        inner = [n for n in (node(c) for c in windows) if n]
        return {"t": "box", "dir": "v", "id": wid, "items": inner} if inner else None
    ovals = [item for item in items if w.type(item) == "oval"]
    if ovals and len(items) <= 2:   # a status dot (RPCN connection)
        return {"t": "dot", "id": wid, "color": w.itemcget(ovals[0], "fill")}
    texts, rects, outline = [], [], ""
    for item in items:
        kind = w.type(item)
        if kind == "text":
            t = w.itemcget(item, "text")
            if t:
                font = ul.tkfont.Font(font=w.itemcget(item, "font")).actual()
                texts.append({"text": t, "fg": w.itemcget(item, "fill"), "size": abs(font.get("size", 10)),
                              "bold": font.get("weight") == "bold", "serif": "Georgia" in font.get("family", "")})
        elif kind in ("rectangle", "polygon"):
            x1, y1, x2, y2 = w.bbox(item) or (0, 0, 0, 0)
            rects.append((x1, x2, w.itemcget(item, "fill"), w.itemcget(item, "outline")))
            # the card's own border is its first shape; later ones (the RECOMMENDED badge's gold pill) don't select it
            if kind == "polygon" and not outline:
                outline = w.itemcget(item, "outline") or "none"
    click = has_binding(w, "<Button-1>")
    if texts or click:
        return {"t": "card", "id": wid, "texts": texts, "click": click, "dclick": has_binding(w, "<Double-Button-1>"),
                "selected": outline.lower() == ul.GOLD.lower()}
    filled = [r for r in rects if r[2] and r[2].lower() in (ul.GOLD.lower(), ul.GOLD_HI.lower(), ul.INK.lower())]
    if filled or rects:
        done = max((r[1] for r in filled), default=0)
        return {"t": "progress", "id": wid, "v": max(0.0, min(1.0, done / width))}
    return None


def children(frame):
    packed, gridded = ordered_children(frame)
    out = []
    for c in packed + gridded:
        n = node(c) if shown(c) else None
        if n:
            out.append(n)
    return out


def chrome(app):
    root = app.root
    status = {"text": text_of(app.status), "detail": text_of(app.substatus)}
    running = app.process is not None
    play = "STOP" if running else ("UPDATING..." if app.u3mp_blocked() else "PLAY")
    games = []
    for key, (canvas, game, _, _) in app.cards.items():
        games.append({"key": key, "number": game["number"], "title": game["title"], "subtitle": game["subtitle"],
                      "selected": key == app.config.get("game")})
    tabs = []
    for name, pill in app.tab_buttons.items():
        cell = app.tab_cells[name]
        tabs.append({"name": name, "label": pill.text, "visible": shown(cell),
                     "selected": name == getattr(app, "current_tab", "")})
    # an update in progress, shown in the bottom bar: one line and a bar, both from the step counts (the Tk banner's
    # canvas is never mapped here, so its drawn width says nothing)
    banner = None
    if getattr(app, "banner_shown", False):
        r3, r2 = getattr(app, "u3mp_block", ""), getattr(app, "u2mp_block", "")
        reason = r3 or r2
        n, total = (tuple(getattr(app, "u3mp_progress", (0, 0, "")))[:2] if r3
                    else tuple(getattr(app, "u2mp_progress", (0, 0)))[:2])
        if reason == "running" and total:
            done = max(0, min(int(n), int(total)))
            banner = {"title": "UPDATE IN PROGRESS - %d/%d (%d%%)" % (done, total, 100 * done // total),
                      "progress": done / total}
        elif reason:
            banner = {"title": "UPDATE STARTING", "progress": 0}
    if getattr(app, "update_busy", False) and getattr(app, "update_release", None):   # a launcher update installing
        done, total = getattr(app, "update_progress", (0, 0))
        pct = " (%d%%)" % (100 * done // total) if total else ""
        banner = {"title": "UPDATING THE LAUNCHER" + pct, "progress": done / total if total else 0}
    update = shown(app.update_button)
    hero = ""
    for name in ("pic1.png", "pic0.png"):
        path = app.game_dir() / "sce_sys" / name
        if path.exists():
            hero = str(path)
            break
    capturing = getattr(app, "capturing", None) is not None
    return {"title": root.title(), "capturing": capturing, "games": games, "tabs": tabs, "status": status, "play": play,
            "running": running, "update": app.update_button.text if update else "", "banner": banner, "hero": hero}


def model(app, tab):
    page = app.pages.get(tab or getattr(app, "current_tab", "setup"))
    items = children(page) if page is not None else []
    return {"chrome": chrome(app), "tab": getattr(app, "current_tab", ""), "page": items}


# -- actions ---------------------------------------------------------------------------------------------------
def widget(app, wid):
    try:
        return app.root.nametowidget(wid)
    except (KeyError, tk.TclError):
        return None


def act(app, message):
    w = widget(app, message.get("id", ""))
    kind, value = message.get("kind"), message.get("value")
    if w is None:
        return
    if kind in ("toggle", "chip"):
        w.set(bool(value), notify=True)
    elif kind in ("choice", "dropdown"):
        options = [str(o) for o in (w.visible if kind == "choice" else [o for o, _ in w.options])]
        real = next((o for o in (w.labels if kind == "choice" else [o for o, _ in w.options]) if str(o) == value), value)
        if str(real) in options:
            w.set(real, notify=True)
    elif kind == "button":
        if w.enabled:
            w.action()
    elif kind == "entry":
        w.delete(0, "end")
        w.insert(0, value or "")
    elif kind == "enter":   # Enter in a text box: what the Tk entry's <Return> does (sign in, add a friend)
        w.event_generate("<Return>", when="now")
    elif kind == "list":
        w.selection_clear(0, "end")
        for i in value or []:
            w.selection_set(int(i))
        w.event_generate("<<ListboxSelect>>")
    elif kind in ("click", "dclick", "rclick"):
        sequence = {"click": "<Button-1>", "dclick": "<Double-Button-1>", "rclick": "<Button-3>"}[kind]
        w.event_generate(sequence, x=4, y=4, when="now")


class KeyEvent:
    def __init__(self, keysym):
        self.keysym, self.char, self.state, self.num, self.delta = keysym, "", 0, 0, 0


def capture(app, message):
    """A key, mouse button or wheel step pressed in the Qt window while a Controls slot waits for one."""
    if getattr(app, "capturing", None) is None:
        return
    if message.get("key"):
        app.capture_key(KeyEvent(message["key"]))
    elif message.get("mouse"):
        app.capture_mouse(message["mouse"])


def main():
    root = tk.Tk()
    root.withdraw()
    root.report_callback_exception = lambda kind, value, trace: (
        ul.log_launcher_error(kind, value, trace), traceback.print_exception(kind, value, trace))
    try:
        ul.ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:  # noqa: BLE001 - not Windows, or set already
        pass
    global _app
    app = _app = ul.Launcher(root)
    root.withdraw()
    threading.Thread(target=reader, daemon=True).start()
    last = {"hash": None}

    def handle(message):
        op = message.get("op")
        if op == "model":
            tab = message.get("tab") or None
            try:
                data = model(app, tab)
            except Exception:  # noqa: BLE001 - the window still gets an answer
                send({"re": "model", "same": True})
                raise
            digest = hashlib.sha1(json.dumps(data, sort_keys=True).encode("utf-8")).hexdigest()
            if digest == last["hash"] and not message.get("force"):
                send({"re": "model", "same": True})
            else:
                last["hash"] = digest
                send({"re": "model", "data": data})
        elif op == "act":
            act(app, message)
        elif op == "capture":
            capture(app, message)
        elif op == "game":
            app.select_game(message.get("key"))
        elif op == "playgame":
            app.play_game(message.get("key"))
        elif op == "tab":
            app.show_tab(message.get("name"))
        elif op == "play":
            app.play_or_stop()
        elif op == "update":
            app.install_update()
        elif op == "close":
            app.close()
            return False
        return True

    def pump():
        try:
            while True:
                message = _held.pop(0) if _held else requests.get_nowait()
                try:
                    if not handle(message):
                        return
                except Exception:  # noqa: BLE001 - one failed request never ends the launcher
                    ul.log_launcher_error(*sys.exc_info())
                    traceback.print_exc()
        except queue.Empty:
            pass
        try:
            root.after(25, pump)
        except tk.TclError:
            pass
    root.after(25, pump)
    send({"ev": "ready"})
    try:
        root.mainloop()
    finally:
        send({"ev": "closed"})


if __name__ == "__main__":
    main()
