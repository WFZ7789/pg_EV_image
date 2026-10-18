import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    baseline, output = args.baseline.resolve(), args.output.resolve()
    manifest = json.loads((baseline / "manifest.json").read_text())
    archive = baseline / "source.tar"
    sha = hashlib.sha256(archive.read_bytes()).hexdigest()
    if sha != manifest["source_sha256"]:
        raise RuntimeError("Frozen source archive hash differs")
    output.mkdir(parents=True, exist_ok=False)
    source = output / "source"
    with tarfile.open(archive) as packed:
        packed.extractall(source, filter="data")
    helper = output / "sensor_reference.cpp"
    shutil.copyfile(root / "tools/legacy_sensor_reference.cpp", helper)
    (output / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 4.0)
project(frozen_sensor_reference LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(HDF5 REQUIRED COMPONENTS C CXX)
find_package(OpenCV REQUIRED)
find_package(CLI11 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(TBB REQUIRED)
add_executable(frozen_sensor_reference sensor_reference.cpp)
target_include_directories(frozen_sensor_reference PRIVATE source/include source/include/legacy
                           ${HDF5_INCLUDE_DIRS} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(frozen_sensor_reference PRIVATE ${HDF5_LIBRARIES} ${HDF5_CXX_LIBRARIES}
                      ${OpenCV_LIBS} CLI11::CLI11 ZLIB::ZLIB TBB::tbb)
''')
    commands = [["cmake", "-S", str(output), "-B", str(output / "build"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"],
                ["cmake", "--build", str(output / "build"), "--target", "frozen_sensor_reference", "-j", "4"]]
    for command in commands:
        subprocess.run(command, check=True)
    executable = output / "build/frozen_sensor_reference"
    (output / "manifest.json").write_text(json.dumps({
        "revision": manifest["revision"], "source_archive_sha256": sha,
        "helper_sha256": hashlib.sha256(helper.read_bytes()).hexdigest(),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "commands": commands,
    }, indent=2) + "\n")
    print(executable)


if __name__ == "__main__":
    main()
