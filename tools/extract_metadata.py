"""Extract plaintext game metadata entries outside the PKG's inner filesystem."""
from pathlib import Path
import json
import re
import subprocess

root = Path(__file__).resolve().parent.parent
pkg = root / "uncharted.pkg"
tool = root / "tools/pkgtool/PkgTool.exe"
game = root / "game/CUSA02344/uroot"
entries = subprocess.check_output([str(tool), "pkg_listentries", str(pkg)], text=True)
paths = {"PARAM_SFO": "param.sfo", "PLAYGO_CHUNK_DAT": "playgo-chunk.dat",
         "PLAYGO_CHUNK_SHA": "playgo-chunk.sha", "PLAYGO_MANIFEST_XML": "playgo-manifest.xml",
         "ICON0_PNG": "icon0.png", "PIC0_PNG": "pic0.png", "PIC1_PNG": "pic1.png",
         "SHAREPARAM_JSON": "shareparam.json",
         "TROPHY__TROPHY01_TRP": "trophy/trophy01.trp",
         "TROPHY__TROPHY02_TRP": "trophy/trophy02.trp",
         "TROPHY__TROPHY03_TRP": "trophy/trophy03.trp"}
report = []
for line in entries.splitlines():
    row = re.match(r"^(0x[\da-fA-F]+)\s+(0x[\da-fA-F]+)\s+([\da-fA-F]{8})\s+(\d+)\s+(?:\d+\s+)?(\S+)$", line)
    if not row or row[5] not in paths:
        continue
    if int(row[3], 16) & 0x80000000:
        raise ValueError(f"Requested metadata is encrypted: {row[5]}")
    target = game / "sce_sys" / paths[row[5]]
    target.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(tool), "pkg_extractentry", str(pkg), row[4], str(target)], check=True)
    assert target.stat().st_size == int(row[2], 16), target
    report.append({"entry": row[5], "index": int(row[4]), "size": target.stat().st_size,
                   "path": str(target.relative_to(game))})
assert {row["entry"] for row in report} == set(paths)
(root / "analysis/metadata-extraction.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
print(f"Extracted {len(report)} metadata files")
