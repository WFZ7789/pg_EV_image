import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    native_path = Path(str(args.native) + ".f32")
    mask_path = Path(str(args.native) + ".u8")
    if native_path.stat().st_size % 4 or native_path.stat().st_size != args.reference.stat().st_size:
        raise RuntimeError("Reference plane dimensions differ")
    count = native_path.stat().st_size // 4
    if not count or mask_path.stat().st_size != count:
        raise RuntimeError("Invalid reference mask size")
    native = np.memmap(native_path, dtype="=f4", mode="r")
    reference = np.memmap(args.reference, dtype="=f4", mode="r")
    mask = np.memmap(mask_path, dtype="u1", mode="r")
    result = {"native": str(native_path), "reference": str(args.reference), "samples": count,
              "common_valid": 0, "native_invalid": 0, "newly_masked_finite_reference": 0,
              "native_valid_reference_nonfinite": 0, "maximum_absolute_error": 0.0,
              "over_tolerance": 0, "absolute_tolerance": 1e-5, "relative_tolerance": 2e-6}
    for start in range(0, count, 1024 * 1024):
        a, b, m = native[start:start + 1024 * 1024], reference[start:start + 1024 * 1024], mask[start:start + 1024 * 1024]
        if np.any(m > 1) or np.any((m != 0) & ~np.isfinite(a)):
            raise RuntimeError("Invalid native validity mask")
        finite = np.isfinite(b)
        common = (m != 0) & finite
        result["common_valid"] += int(common.sum())
        result["native_invalid"] += int((m == 0).sum())
        result["newly_masked_finite_reference"] += int(((m == 0) & finite).sum())
        result["native_valid_reference_nonfinite"] += int(((m != 0) & ~finite).sum())
        difference = np.abs(a[common].astype("f8") - b[common])
        tolerance = 1e-5 + 2e-6 * np.abs(b[common].astype("f8"))
        result["over_tolerance"] += int((difference > tolerance).sum())
        if difference.size:
            result["maximum_absolute_error"] = max(result["maximum_absolute_error"], float(difference.max()))
    result["sha256"] = {str(path): digest(path) for path in (native_path, mask_path, args.reference)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if result["over_tolerance"] or result["native_valid_reference_nonfinite"] or not result["common_valid"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
