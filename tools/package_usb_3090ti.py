"""Assemble a FAT32-ready USB tree for the RTX 3090 Ti UEFI port.

This never selects, mounts or formats a drive: it writes a folder that you copy
to the root of an already-formatted FAT32 USB stick, exactly like the upstream
package_usb.py flow.

Variants
  --variant profile   EFI/BOOT/BOOTX64.EFI is the read-only profiler
  --variant unlock    EFI/BOOT/BOOTX64.EFI is the unlocker (assets required)

Examples
  python tools/package_usb_3090ti.py --variant profile --output .\\usb-profile
  python tools/package_usb_3090ti.py --variant unlock --assets .\\assets --output .\\usb-unlock
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

PROFILE_EFI = Path("build/TI_PROFILE.EFI")
UNLOCK_EFI = Path("unlock/build/BOOTX64.EFI")
PROBE_DIR = "RTX3090TI-PROBE"
PAYLOAD_DIR = "RTX3090TI-UNLOCK"


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def check_efi(path):
    """Reject anything that is not a PE32+ EFI application before it is shipped."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("not a PE image: " + str(path))
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 24 > len(data) or data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("missing PE header: " + str(path))
    magic, subsystem = struct.unpack_from("<H", data, pe + 24)[0], struct.unpack_from("<H", data, pe + 24 + 68)[0]
    if magic != 0x20B:
        raise ValueError("not PE32+ (0x%x): %s" % (magic, path))
    if subsystem != 10:
        raise ValueError("subsystem %d is not EFI application (10): %s" % (subsystem, path))


def load_asset_specs(root):
    return json.loads((root / "unlock/assets.json").read_text(encoding="utf-8-sig"))


def prepare(root, variant, assets, output):
    root = root.resolve()
    output = output.absolute()
    if output.exists():
        raise ValueError("Output already exists; choose a new local directory.")
    if not output.parent.is_dir():
        raise ValueError("Output parent must be an existing local directory.")

    efi = (root / (PROFILE_EFI if variant == "profile" else UNLOCK_EFI)).resolve()
    if not efi.is_file():
        raise ValueError("Missing %s; run unlock/build-wsl.sh first." % efi)
    check_efi(efi)

    specs = load_asset_specs(root)
    if variant == "unlock" and assets is None:
        raise ValueError("The unlocker needs the five firmware assets; pass --assets.")
    if assets is not None:
        assets = assets.resolve()
        for entry in specs:
            path = assets / entry["Name"]
            if not path.is_file() or path.stat().st_size != entry["Bytes"] or digest(path) != entry["Sha256"].lower():
                raise ValueError("Missing or changed asset: " + entry["Name"])

    boot = output / "EFI/BOOT"
    probe = output / PROBE_DIR
    payload = output / PAYLOAD_DIR
    for directory in (boot, probe, payload / "ASSETS", payload / "LICENSES"):
        directory.mkdir(parents=True, exist_ok=True)

    target = boot / "BOOTX64.EFI"
    target.write_bytes(efi.read_bytes())
    if digest(target) != digest(efi):
        raise ValueError("EFI changed while copying.")

    if variant == "unlock":
        for entry in specs:
            (payload / "ASSETS" / entry["Name"]).write_bytes((assets / entry["Name"]).read_bytes())
            if digest(payload / "ASSETS" / entry["Name"]) != entry["Sha256"].lower():
                raise ValueError("Asset changed while copying: " + entry["Name"])
    else:
        (payload / "ASSETS/REQUIRED-ASSETS.json").write_text(
            json.dumps(specs, indent=2) + "\n", encoding="utf-8"
        )

    summary = {
        "variant": variant,
        "efi": str(efi),
        "efi_sha256": digest(target),
        "assets_included": variant == "unlock",
        "boot_order": [
            "1. Firmware boot menu -> USB (UEFI, not legacy)",
            "2. Wait for the report / READY block on screen",
            "3. profile: report is saved, then choose Windows Boot Manager",
            "   unlock:  press U once, then choose Windows Boot Manager WITHOUT rebooting",
        ],
        "profile_report": "/%s/RUN00/REPORT.TXT" % PROBE_DIR,
        "unlock_report": "/%s/RUN00/REPORT.TXT" % PAYLOAD_DIR,
    }
    (output / "PACKAGE.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    sums = "".join(
        digest(p) + "  " + p.relative_to(output).as_posix() + "\n"
        for p in sorted(output.rglob("*"))
        if p.is_file()
    )
    (output / "SHA256SUMS.txt").write_text(sums, encoding="utf-8")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--variant", choices=("profile", "unlock"), required=True)
    parser.add_argument("--assets", type=Path, help="Directory with the five matching firmware assets.")
    parser.add_argument("--output", type=Path, required=True, help="New local directory; never a drive.")
    args = parser.parse_args()
    try:
        summary = prepare(args.project, args.variant, args.assets, args.output)
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    sys.exit(main())
