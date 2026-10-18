import argparse
import hashlib
import json
from pathlib import Path

import cv2
import numpy as np


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("baseline", type=Path)
    args = parser.parse_args()
    root = args.baseline.resolve()
    hashes = json.loads((root / "reference_hashes.json").read_text())
    result = {"images": {}, "frames": {}}
    for name, expected in hashes.items():
        path = root / name
        if digest(path) != expected:
            raise RuntimeError("Reference hash mismatch: " + name)
        if path.suffix in (".png", ".tif"):
            image = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
            if image is None or image.size == 0:
                raise RuntimeError("Cannot decode image: " + name)
            expected_type = np.uint16 if "rtec16" in name else np.uint8
            if image.dtype != expected_type or image.ndim != 3 or image.shape[2] != 3:
                raise RuntimeError("Unexpected image type: " + name)
            result["images"][name] = {"shape": list(image.shape), "dtype": str(image.dtype)}
            del image
    for path in sorted(root.glob("*.frame*")):
        if not path.is_dir():
            continue
        metadata = json.loads((path / "frame.json").read_text())
        count = metadata["rows"] * metadata["cols"]
        byte_order = "<" if metadata["endian"] == "little" else ">"
        planes = {}
        for name in ("latitude", "longitude", "values"):
            plane = path / (name + ".f32")
            expected_count = int(metadata["value_count"]) if name == "values" else count
            if plane.stat().st_size != 4 * expected_count:
                raise RuntimeError("Incorrect plane length: " + str(plane))
            values = np.memmap(plane, dtype=byte_order + "f4", mode="r")
            finite_count = 0
            low, high = float("inf"), float("-inf")
            for start in range(0, values.size, 1024 * 1024):
                block = values[start:start + 1024 * 1024]
                finite = block[np.isfinite(block)]
                finite_count += finite.size
                if finite.size:
                    low, high = min(low, float(finite.min())), max(high, float(finite.max()))
            planes[name] = {"count": values.size, "finite": finite_count,
                            "min": low if finite_count else None, "max": high if finite_count else None}
        if (path / "color.bgr8").stat().st_size != count * 3:
            raise RuntimeError("Incorrect color plane length: " + str(path))
        mask_bytes = (path / "valid.u8").stat().st_size
        if mask_bytes not in (0, count):
            raise RuntimeError("Incorrect mask length: " + str(path))
        lat = np.memmap(path / "latitude.f32", dtype=byte_order + "f4", mode="r")
        lon = np.memmap(path / "longitude.f32", dtype=byte_order + "f4", mode="r")
        invalid_geo = 0
        for start in range(0, count, 1024 * 1024):
            la, lo = lat[start:start + 1024 * 1024], lon[start:start + 1024 * 1024]
            valid = np.isfinite(la) & np.isfinite(lo) & (np.abs(la) <= 90) & (lo >= -180) & (lo <= 360)
            invalid_geo += int((~valid).sum())
        result["frames"][path.name] = {"rows": metadata["rows"], "cols": metadata["cols"],
                                      "planes": planes, "invalid_geo_count": invalid_geo}
    manifest = json.loads((root / "manifest.json").read_text())
    if digest(root / "source.tar") != manifest["source_sha256"]:
        raise RuntimeError("Source archive changed")
    if digest(root / "build/pg_EV_image") != manifest["executable_sha256"]:
        raise RuntimeError("Frozen executable changed")
    (root / "validation.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Validated", len(result["images"]), "lossless images and", len(result["frames"]), "numerical frames")


if __name__ == "__main__":
    main()
