# Uncharted Launcher: Qt window

The launcher's window in Qt 6 (C++ and QML). The launcher itself is the native core in
`launcher/native/uncharted_native.dll` (`core.cpp`), which this program loads and runs in its own process; there is
no Python part.

## How it works

- `src/backend.cpp` opens the core (`un_core_open`), sends it each request as a JSON message (`un_core_send`) and
  calls its timers about every 50 ms (`un_core_tick`). The core answers with the window's model and events.
- The model: game cards, tabs, status line, PLAY, the update line, and the shown tab's page. Pages are trees of
  sections, rows and controls that `pages.cpp` builds (`un_page`); a click goes back to `un_page_act`, which says
  what changes.
- Questions and messages arrive as dialog events: message boxes are QML dialogs, file and folder pickers native Qt
  dialogs, and the answer goes back to the core, which carries on from there.
- `--play u1 | u2 | u3` starts that game at once; the window stays hidden unless the core asks something, and the
  program ends with the game.

## Editing the look

- `qml/Theme.qml`: every colour, radius, size and font.
- `qml/Main.qml`: the window layout (picture, game cards, tabs, page, PLAY bar).
- `qml/PageView.qml`: how a page's sections are placed (two columns from 900 px wide).
- `qml/SectionCard.qml`, `qml/SettingRow.qml`, `qml/NodeView.qml`: a section card, one setting row, and which
  control draws which element.
- The controls: `PillButton`, `ToggleSwitch`, `ChoiceBar`, `DropdownBox`, `ChipButton`, `InputField`, `LogView`,
  `ProgressLine`, `PresetCard`, `PadIcon`, `MessageDialog`.

Open `CMakeLists.txt` in Qt Creator with the Qt 6 kit. The QML files also open in Qt Design Studio. A page's
sections and rows are written in `launcher/native/pages.cpp`.

## Building (Windows)

`tools/package_windows.sh` builds it with MSYS2 CLANG64's Qt. By hand:

```
cmake -S launcher-qt -B build-qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C build-qt UnchartedLauncher
```

## Running

The program looks for `launcher/native/uncharted_native.dll` in its own folder and up to five folders above. For
development:

- `UNCHARTED_LAUNCHER_ROOT`: the launcher folder.
- `UNCHARTED_QT_TAB`: open on this tab.
