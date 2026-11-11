"""Pinned v0.6 write-sequence regression; host model only, no GPU access."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys

root = Path(__file__).resolve().parents[1]
build = root / "build"
reference = root / "tests/reference/unlock_610_v06.c"
# Same-line attribution notice added to the public v0.6 snapshot; code and line numbers are unchanged.
reference_sha = "1440F0D0C974B8F1D985F48B5852BE7287ABE10BC66D468FCA3833E3CDBC9BAB"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


if sha(reference) != reference_sha:
    raise ValueError("Immutable v0.6 reference changed")
if sys.argv[1:] == ["--prepare"]:
    stage = build / "reference-v06"
    (stage / "vendor").mkdir(parents=True, exist_ok=True)
    shutil.copyfile(reference, stage / "vendor/unlock_610.c")
    # Resolve the archived relative include to the same pragma-once file.
    (stage / "port.h").write_text('#include "../../port.h"\n', encoding="ascii")
    print("Pinned v0.6 reference prepared")
    sys.exit(0)
if sys.argv[1:]:
    raise ValueError("Unknown arguments")
current = build / "fwsec-current.trace"
baseline = build / "fwsec-v06.trace"
expected = [(0, 0), (13, -5), (1, -110), (18, -5), (19, -5)]


def parse(path, name):
    data = path.read_bytes()
    if len(data) < 8 or struct.unpack_from("<II", data) != (0x47555431, 5):
        raise ValueError(f"Incomplete {name} trace header")
    at = 8
    scenarios = []
    for expected_mode, expected_result in expected:
        mode, result, count = struct.unpack_from("<IiI", data, at)
        at += 12
        if (mode, result) != (expected_mode, expected_result) or count == 0:
            raise ValueError(f"Unexpected {name} trace scenario/outcome")
        records = [struct.unpack_from("<IIII", data, at + i * 16) for i in range(count)]
        if any(kind not in (0, 1) or (kind == 0 and offset in (0x1101c0, 0x1101c4))
               for kind, width, offset, value in records):
            raise ValueError(f"Unexpected {name} write type or PIO access")
        at += count * 16
        scenarios.append((mode, result, records))
    if at != len(data):
        raise ValueError(f"Trailing or truncated {name} trace")
    return scenarios


now, then = parse(current, "current"), parse(baseline, "v0.6")
cases = []
for (mode, result, new), (_, _, old) in zip(now, then):
    if mode == 0:
        # The pinned unlock sequence must be byte-identical on success.
        if new != old:
            raise ValueError("Success-path MMIO/PCI writes differ from v0.6")
    else:
        # Failure containment is new: v0.6 ended at the failure write, the
        # current core appends the bounded abort FLR after that point.
        if new[:len(old)] != old or len(new) <= len(old):
            raise ValueError(f"Failure scenario {mode} writes diverge from v0.6 prefix")
    cases.append({"Scenario": mode, "Result": result, "OrderedWrites": len(new)})
record = {"AllMatched": True, "HardwareAccess": False,
          "ReferenceCoreSha256": reference_sha,
          "CurrentCoreSha256": sha(root / "vendor/unlock_610.c"),
          "TraceSha256": sha(current), "Scenarios": cases,
          "Scope": "Current versus archived v0.6 core using the SAME current port/compat and simulated EFI services. Not historical adapter equivalence or physical bus-driver transactions. Extra reads/logging may change timing."}
(build / "fwsec-sequence-verification.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
print("5 core write sequences verified against archived v0.6 (success identical, failures append the abort FLR) using the same current adapter; hardware behavior simulated.")
