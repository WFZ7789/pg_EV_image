import hashlib
import json
from pathlib import Path


def main():
    directory = Path(__file__).parent / "reference_v1"
    manifest = json.loads((directory / "manifest.json").read_text())
    expected = {"swath": 29, "fy4": 47, "pmr": 4, "giirs": 7, "spectrum": 12}
    names = {f"{group}-{index}.bin" for group, count in expected.items() for index in range(count)}
    assert {item["name"] for item in manifest["files"]} == names
    assert {path.name for path in directory.glob("*.bin") if not path.name.startswith("._")} == names
    assert manifest["baseline_revision"] == "aaf6eef09e45029a3d5c5e7348339727eabb62a3"
    for item in manifest["files"]:
        content = (directory / item["name"]).read_bytes()
        assert content.startswith(b"FYREF001")
        assert len(content) == item["size"]
        assert hashlib.sha256(content).hexdigest() == item["sha256"]
    print("99 independent frozen product references verified")


if __name__ == "__main__":
    main()
