"""Inventory plaintext Uncharted PS4 executables using the base runtime's parser.

This is static inspection, not proof that an import is implemented or a game boots.
"""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "gow3_pc" / "scripts"))
from link_modules import canonical, module
from prepare import sfo


def inspect(game, out):
    out.mkdir(parents=True, exist_ok=True)
    known = dict(re.findall(r'\{"([^"]+)",\s*"([^"]+)"\}',
                           (ROOT / "gow3_pc/src/import_names.inc").read_text()))
    report = {"authority": "local artifact bytes and source parser",
              "limitations": ["No execution observed by this inventory.",
                              "A known symbol name does not prove runtime support.",
                              "SELF reconstruction omits unavailable non-loadable metadata."],
              "sfo": sfo((game / "sce_sys/param.sfo").read_bytes()), "executables": []}
    for filename in ("eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"):
        path = game / filename
        data = module(path)
        imports = []
        for symbol in data["symbols"]:
            if symbol["section"] or not symbol["name"]:
                continue
            name = canonical(symbol["identity"], symbol["name"])
            imports.append({"name": name, "hint": known.get(name),
                            "kind": "data" if symbol["type"] == 1 else "function"})
        destination = out / (filename + ".reconstructed.elf")
        destination.write_bytes(data["elf"])
        report["executables"].append({
            "file": filename, "source_sha256": data["sha256"],
            "reconstructed_sha256": hashlib.sha256(data["elf"]).hexdigest(),
            "entry": hex(data["header"][4]), "imports": imports,
            "import_count": len(imports), "relocation_count": len(data["relocs"]),
            "libraries": dict(collections.Counter(
                row["name"].split("#", 1)[-1] for row in imports)),
            "unavailable_metadata_headers": data["missing"],
        })
    (out / "inventory.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    lines = ["# Uncharted initial inventory", "",
             f"Title: {report['sfo']['TITLE']}", f"Version: {report['sfo']['APP_VER']}", "",
             "Static inspection only; runtime compatibility has not been established.", ""]
    for exe in report["executables"]:
        lines += [f"## {exe['file']}", "",
                  f"Entry: `{exe['entry']}`; {exe['import_count']} imports; {exe['relocation_count']} relocations.", "",
                  "| Imported library | Symbols |", "| --- | ---: |"]
        lines += [f"| {lib} | {count} |" for lib, count in sorted(exe["libraries"].items())]
        lines += [""]
    (out / "inventory.md").write_text("\n".join(lines), encoding="utf-8")
    print("\n".join(f"{exe['file']}: {exe['import_count']} imports" for exe in report["executables"]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=ROOT / "game/CUSA02344/uroot")
    parser.add_argument("--out", type=Path, default=ROOT / "analysis/inventory")
    args = parser.parse_args()
    inspect(args.game.resolve(), args.out.resolve())
