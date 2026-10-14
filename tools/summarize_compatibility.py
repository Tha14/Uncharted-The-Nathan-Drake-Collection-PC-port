"""Summarize actual local runtime import-check logs, keeping failures explicit."""
import collections
import csv
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
ANALYSIS = ROOT / "analysis"
targets = {"eboot.bin": "uncharted", "big2-ps4_Shipping.elf": "big2-ps4_Shipping",
           "big3-ps4_Shipping.elf": "big3-ps4_Shipping"}
report = {}
for executable, folder in targets.items():
    path = ANALYSIS / folder
    result = json.loads((path / "check-imports.json").read_text())
    log = (path / "check-imports.log").read_text(errors="replace")
    count = re.search(r"^Imports: (\d+) missing$", log, re.M)
    if not count:
        raise RuntimeError(f"Import check did not complete: {folder}")
    missing = []
    for name, hint in re.findall(r"^MISSING: (\S+) \((.*)\)$", log, re.M):
        missing.append({"name": name, "library": name.split("#")[-1], "hint": hint})
    assert len(missing) == int(count[1])
    report[executable] = {"missing_count": len(missing), "missing": missing,
                          "by_library": dict(collections.Counter(row["library"] for row in missing)),
                          "probe_result": result}
    with (path / "missing-imports.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("name", "library", "hint"))
        writer.writeheader()
        writer.writerows(missing)
(ANALYSIS / "compatibility.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
lines = ["# Uncharted runtime compatibility baseline", "",
         "Observed on 13 October 2026. Package: CUSA02344, version 01.00.", "",
         "The package was extracted and the Windows runtime built from the local God of War source.",
         "The main executable enters original guest code and completes both bundled-library initializers.",
         "No game menu or gameplay has been observed.", "",
         "## Current startup blocker", "",
         "The latest bounded boot stops at `sceFiberInitialize` (`hVYD7Ou2pCQ#libSceFiber`),",
         "with caller return offset `0x363c5d`. Real fiber creation and context switching are not implemented.",
         "This needs stack/CPU-context preservation and per-thread fiber state; returning success would be incorrect.", "",
         "## Import checks", "", "| Executable | Missing runtime imports |", "| --- | ---: |"]
lines += [f"| {name} | {item['missing_count']} |" for name, item in report.items()]
for name, item in report.items():
    lines += ["", f"### {name}", "", "| Library | Missing symbols |", "| --- | ---: |"]
    lines += [f"| {lib} | {count} |" for lib, count in sorted(item["by_library"].items())]
lines += ["", "## Implemented and verified", "",
          "- Added `--no-game-hooks` to skip the base project's gameplay hooks.",
          "- Implemented memory-range naming with stored VMA names and query visibility.",
          "- Implemented the fiber option initializer, including pointer checks and its signature.",
          "- Added executable selection to preparation/linking for all three game binaries.",
          "- Both focused native tests passed; the Windows runtime build passed.",
          "- The main boot passes the formerly missing memory-name and fiber-option calls.", "",
          "## Reproduce", "", "From this workspace in PowerShell:", "", "```powershell",
          "python tools/inspect_uncharted.py",
          "python tools/probe_uncharted.py",
          "python tools/probe_uncharted.py --executable big2-ps4_Shipping.elf",
          "python tools/probe_uncharted.py --executable big3-ps4_Shipping.elf",
          "python tools/probe_uncharted.py --boot --seconds 20",
          "python tools/summarize_compatibility.py", "```", "",
          "Checks returning exit code 1 report missing imports. The boot currently returns 20 at the unsupported fiber call.",
          "The external timeout kills only the probe process if it exceeds the selected time.",
          "The shell sandbox failed address reservation before execution; successful probes ran outside that sandbox.", "",
          "Build/test inside an MSYS2 CLANG64 shell:", "", "```bash", "cd gow3_pc", "bash build.sh",
          "ninja -C out/gpu range-names-test fiber-opt-test",
          "out/gpu/range-names-test.exe", "out/gpu/fiber-opt-test.exe", "```", "",
          "## Evidence and limitations", "",
          "- `uncharted/boot.log` and `boot.json`: actual bounded main-executable execution.",
          "- Each executable's `check-imports.log`, JSON and CSV: actual resolver coverage, not ABI correctness.",
          "- `inventory/inventory.json`: static import/library inventory and source/reconstructed hashes.",
          "- `extract.log`, `pkg-entries.txt`, `rebuild.log`: extraction and build/test records.",
          "- REA rejected the PS4 PKG as unsupported. It opened the reconstructed x86-64 ELF",
          "  (SHA-256 `ac8fcc264b48abcd2f41671ca728246a626b5c8a148299bac788adaff0b5bd73`),",
          "  but decompilation was unavailable: its Windows Ghidra integration accepts PE only;",
          "  Hopper is unavailable on Windows; IDA is not configured. No REA decompilation findings are claimed.",
          "- SELF reconstruction omits unavailable non-loadable metadata; it is not a byte-exact original ELF.",
          "- The base GPU code retains God of War-specific behavior that still needs review for Uncharted.",
          "- The Uncharted 2/3 executables were checked for imports; guest execution was tested only for `eboot.bin`.",
          "- No complete PKG integrity validation or comparison against an independent game dump was performed.", "",
          "API references: [shadPS4 memory](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp),",
          "[shadPS4 fiber](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/fiber/fiber.cpp).", ""]
(ANALYSIS / "IMPORTS-BASELINE.md").write_text("\n".join(lines), encoding="utf-8")
print("\n".join(f"{name}: {item['missing_count']} missing imports" for name, item in report.items()))
