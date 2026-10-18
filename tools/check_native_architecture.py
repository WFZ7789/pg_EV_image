import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    commands = json.loads((build / "compile_commands.json").read_text())
    obsolete = ("src/legacy.cpp", "src/mersi.cpp", "src/pipeline.cpp", "include/legacy/", "legacy_bridge.hpp", "fy_pipeline")
    forbidden = re.compile(r"CapturedScene|active_scene_capture|legacy_main|run_legacy|read_mersi|GeoProjection|fy::Raster<|fy::ProductFrame")
    sources = [path for folder in ("src", "include") for path in (root / folder).rglob("*")
               if path.is_file() and not path.name.startswith("._") and path.suffix in (".cpp", ".hpp", ".h")]
    render_sources = [root / name for name in ("src/render_product.cpp", "src/render_detail.hpp",
                                               "src/core/render.cpp", "src/core/image_render.cpp")]
    payload_literal = re.compile(r'"(?:mersi|mwri|pmr|giirs|fy4|FY-[34][A-Z]?|AGRI|GHI)(?:"|[^a-zA-Z])', re.IGNORECASE)
    for source in render_sources:
        if payload_literal.search(source.read_text()):
            raise RuntimeError("Payload-specific literal in generic renderer: " + str(source))
    for source in sources:
        text = source.read_text()
        if forbidden.search(text):
            raise RuntimeError("Obsolete execution code: " + str(source))
        if source.parent != root / "src/sensors" and re.search(r"H5::|#include\s*[<\"](?:H5|hdf5)", text):
            raise RuntimeError("HDF5 escaped its adapter implementation: " + str(source))
        if any(part in ("core", "products") for part in source.relative_to(root).parts) and re.search(r"cv::|opencv|CLI::|H5::", text):
            raise RuntimeError("External application types escaped into compute APIs: " + str(source))
    for entry in commands:
        if any(name in entry["command"] or name in entry["file"] for name in obsolete):
            raise RuntimeError("Obsolete compilation dependency: " + entry["file"])
    for name in ("include/legacy_bridge.hpp", "include/raster.hpp", "src/legacy.cpp", "src/mersi.cpp", "src/pipeline.cpp"):
        if (root / name).exists():
            raise RuntimeError("Obsolete source remains: " + name)
    if any(not path.name.startswith("._") for path in (root / "include/legacy").glob("*.h")):
        raise RuntimeError("Legacy headers remain")
    executable = build / "pg_EV_image"
    symbols = subprocess.check_output(["nm", "-a", str(executable)], text=True)
    if re.search(r"CapturedScene|active_scene_capture|legacy_main|run_legacy|read_mersi|GeoProjection|N2fy6Raster|N2fy12ProductFrame", symbols):
        raise RuntimeError("Obsolete symbols remain in the executable")
    result = {"build": str(build), "source_files_checked": len(sources), "translation_units_checked": len(commands),
              "legacy_source_dependencies": 0, "legacy_symbols": 0,
              "generic_renderer_files_checked": len(render_sources), "payload_specific_renderer_literals": 0,
              "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
