#!/usr/bin/env python3
"""Rebuild and install the previously validated Outdoor/Hunter Jacket A repair."""
from pathlib import Path
import hashlib, json, os, shutil, sys

sys.dont_write_bytecode = True
sys.path.insert(0, r"{{PAKTOOLS}}")
sys.path.insert(0, r"{{SCRATCH}}/actor-materials")
from repair_hunter_jacket import NAME, SOURCE_SHA256, build_repair, pak_gates

WORK = Path(r"{{WORK}}")
SCRATCH = Path(r"{{SCRATCH}}")
REPAIRED_SHA256 = "a28e8d1bf48ccd338ea7598fa67a91a2c717d539c412460872e4b08680f2c22c"
REL = Path("u3data/build/main/actor33/ps4")
SOURCE = WORK / "overlay-r3" / REL / (NAME + ".pak")
PS3_SOURCE = SCRATCH / "hat-colour/ps3" / (NAME + ".pak")
CANDIDATE = SCRATCH / "outdoor-jacket/out" / (NAME + ".pak")
BACKUP_DIR = WORK / "backup-outdoor-jacket"
DONOR = BACKUP_DIR / "donor.pak"
BACKUP = BACKUP_DIR / (NAME + ".pak")
STATE = BACKUP_DIR / "state.json"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    for path in (SOURCE, DONOR, PS3_SOURCE):
        if not path.is_file():
            raise SystemExit("missing required jacket input: " + str(path))
    current = sha(SOURCE.read_bytes())
    if current == REPAIRED_SHA256:
        print(NAME, "already repaired")
        return
    if current != SOURCE_SHA256:
        raise SystemExit("unexpected active jacket pak SHA-256: " + current)

    candidate, report = build_repair(
        SOURCE.read_bytes(), DONOR.read_bytes(), PS3_SOURCE.read_bytes(), reconstruct_basis=True
    )
    candidate_hash = sha(candidate)
    if candidate_hash != REPAIRED_SHA256:
        raise SystemExit("unexpected repaired jacket SHA-256: " + candidate_hash)

    CANDIDATE.parent.mkdir(parents=True, exist_ok=True)
    if CANDIDATE.exists():
        if sha(CANDIDATE.read_bytes()) != REPAIRED_SHA256:
            raise SystemExit("refusing to overwrite unexpected candidate: " + str(CANDIDATE))
    else:
        CANDIDATE.write_bytes(candidate)
    gates = pak_gates(SOURCE, CANDIDATE)
    print("jacket gates:", json.dumps(gates, sort_keys=True))

    BACKUP_DIR.mkdir(parents=True, exist_ok=True)
    if BACKUP.exists():
        if sha(BACKUP.read_bytes()) != SOURCE_SHA256:
            raise SystemExit("existing Outdoor Jacket backup has an unexpected hash")
    else:
        shutil.copy2(SOURCE, BACKUP)
    if sha(SOURCE.read_bytes()) != SOURCE_SHA256:
        raise SystemExit("active jacket changed during repair")

    temp = SOURCE.with_name(SOURCE.name + ".outdoor-jacket.tmp")
    if temp.exists():
        raise SystemExit("refusing to overwrite stale temporary pak: " + str(temp))
    temp.write_bytes(candidate)
    os.replace(temp, SOURCE)
    if sha(SOURCE.read_bytes()) != REPAIRED_SHA256:
        raise SystemExit("installed jacket hash verification failed")
    STATE.write_text(json.dumps({"asset": NAME, "source_sha256": SOURCE_SHA256,
                                 "repaired_sha256": REPAIRED_SHA256,
                                 "candidate_bytes": report["candidate_bytes"],
                                 "vram_bytes": report["vram_bytes"],
                                 "basis_method": report["basis_method"],
                                 "donor_sha256": sha(DONOR.read_bytes()),
                                 "ps3_source_sha256": sha(PS3_SOURCE.read_bytes())}, indent=2) + "\n",
                     encoding="utf-8")
    print(NAME, "installed", REPAIRED_SHA256)


if __name__ == "__main__":
    main()
