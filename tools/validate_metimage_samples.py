import argparse
import ctypes as C
import ctypes.util
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

import numpy as np


CHANNELS = (443, 555, 668, 752, 763, 865, 914, 1240, 1375, 1630, 2250,
            3740, 3959, 4050, 6725, 7325, 8540, 10690, 12020, 13345)
MEASUREMENT = "/data/measurement_data/"
CALIBRATION = "/data/calibration_data/"
QUALITY = "/data/quality_information/"
PROCESSING = "/data/processing_flags/"


class NetcdfOracle:
    def __init__(self, path):
        with zipfile.ZipFile(path) as package:
            payload = package.read(next(n for n in package.namelist() if n.endswith(".nc")))
        self.buffer = C.create_string_buffer(payload)
        self.lib = C.CDLL(ctypes.util.find_library("netcdf"))
        self.lib.nc_strerror.restype = C.c_char_p
        self.lib.nc_open_mem.argtypes = [C.c_char_p, C.c_int, C.c_size_t, C.c_void_p, C.POINTER(C.c_int)]
        self.handle = C.c_int()
        self.call("nc_open_mem", b"independent-oracle.nc", 0, len(payload), self.buffer, C.byref(self.handle))
        self.variables = {}
        self.cache = {}
        self.shape = self.variable(MEASUREMENT + "vii_443")[2]

    def close(self):
        self.call("nc_close", self.handle)

    def call(self, name, *args):
        result = getattr(self.lib, name)(*args)
        if result:
            raise RuntimeError(name + ": " + self.lib.nc_strerror(result).decode())

    def variable(self, path):
        if path not in self.variables:
            parent, name = path.rsplit("/", 1)
            group, variable, rank = C.c_int(), C.c_int(), C.c_int()
            self.call("nc_inq_grp_full_ncid", self.handle, parent.encode(), C.byref(group))
            self.call("nc_inq_varid", group, name.encode(), C.byref(variable))
            self.call("nc_inq_varndims", group, variable, C.byref(rank))
            dimensions = (C.c_int * rank.value)()
            self.call("nc_inq_vardimid", group, variable, dimensions)
            shape = []
            for dimension in dimensions:
                length = C.c_size_t()
                self.call("nc_inq_dimlen", group, dimension, C.byref(length))
                shape.append(length.value)
            self.variables[path] = group, variable, tuple(shape)
        return self.variables[path]

    def attribute(self, group, variable, name, default=None):
        length = C.c_size_t()
        result = self.lib.nc_inq_attlen(group, variable, name.encode(), C.byref(length))
        if result:
            if default is not None:
                return np.array(default, dtype=np.float64)
            self.call("nc_inq_attlen", group, variable, name.encode(), C.byref(length))
        values = np.empty(length.value, dtype=np.float64)
        self.call("nc_get_att_double", group, variable, name.encode(), C.c_void_p(values.ctypes.data))
        return values

    def root_text(self, name):
        value = C.c_char_p()
        self.call("nc_get_att_string", self.handle, -1, name.encode(), C.byref(value))
        result = value.value.decode()
        self.call("nc_free_string", C.c_size_t(1), C.byref(value))
        return result

    def root_number(self, name):
        return self.attribute(self.handle, -1, name)[0]

    def read(self, path, origin=None, shape=None):
        group, variable, dimensions = self.variable(path)
        if origin is None:
            origin, shape = (0,) * len(dimensions), dimensions
        result = np.empty(shape, dtype=np.float64)
        if dimensions:
            begin = (C.c_size_t * len(dimensions))(*origin)
            count = (C.c_size_t * len(dimensions))(*shape)
            self.call("nc_get_vara_double", group, variable, begin, count, C.c_void_p(result.ctypes.data))
        else:
            self.call("nc_get_var_double", group, variable, C.c_void_p(result.ctypes.data))
        return result

    def all(self, path):
        if path not in self.cache:
            self.cache[path] = self.read(path)
        return self.cache[path]

    def packed(self, path, origin=None, shape=None):
        group, variable, _ = self.variable(path)
        raw = self.read(path, origin, shape)
        valid = np.isfinite(raw)
        for fill in self.attribute(group, variable, "_FillValue", []):
            valid &= raw != fill
        valid &= raw >= self.attribute(group, variable, "valid_min", [-np.inf])[0]
        valid &= raw <= self.attribute(group, variable, "valid_max", [np.inf])[0]
        physical = raw * self.attribute(group, variable, "scale_factor", [1])[0]
        physical += self.attribute(group, variable, "add_offset", [0])[0]
        return physical, valid

    def indices(self, window):
        row, col, rows, cols = window
        return np.broadcast_arrays(np.arange(row, row + rows)[:, None], np.arange(col, col + cols)[None, :])

    def channel(self, band, window):
        row, col, rows, cols = window
        radiance, valid = self.packed(MEASUREMENT + f"vii_{CHANNELS[band]}", (row, col), (rows, cols))
        r, c = self.indices(window)
        scan, detector = r // 24, r % 24
        data = self.all(QUALITY + "data_quality").astype(np.uint32)[scan, band]
        proc = self.all(PROCESSING + "vii_processing_flag").astype(np.uint32)[scan, band]
        geo = self.all(QUALITY + "geolocation_quality").astype(np.uint32)[scan]
        valid &= (data & 0x15) == 0
        valid &= (proc & 1) == 0
        valid &= (geo & 0x57) == 0
        valid &= self.all(PROCESSING + "pixel_duplication_mask")[detector, c] != 1
        valid &= self.all(PROCESSING + "pixel_performance_mask")[band, detector] == 0
        if band < 11:
            calibration = self.all(QUALITY + "calibration_quality").astype(np.uint32)[scan, band]
            valid &= (calibration & 0x30) == 0
            distance = self.all("/status/satellite/earth_sun_distance_ratio").item()
            values = np.pi * radiance * distance ** 2 / self.all(CALIBRATION + "band_averaged_solar_irradiance")[band]
        else:
            wavelength = self.all(CALIBRATION + "channel_cw_thermal")[band - 11] * 1e-6
            a = self.all(CALIBRATION + "bt_conversion_a")[band - 11]
            b = self.all(CALIBRATION + "bt_conversion_b")[band - 11]
            valid &= radiance > 0
            with np.errstate(invalid="ignore", divide="ignore"):
                values = a * 6.626069e-34 * 299792458 / (1.38065e-23 * wavelength * np.log1p(
                    2 * 6.626069e-34 * 299792458 ** 2 / (wavelength ** 5 * radiance * 1e6))) + b
        valid &= np.isfinite(values)
        return values, valid

    def tie_support(self, window):
        r, c = self.indices(window)
        top, left = 4 * (r // 24) + (r % 24) // 8, c // 8
        u, v = (r % 8) / 8, (c % 8) / 8
        corners = ((top, left), (top, left + 1), (top + 1, left), (top + 1, left + 1))
        return corners, ((1 - u) * (1 - v), (1 - u) * v, u * (1 - v), u * v)

    def interpolate(self, field, window, cyclic=False):
        values, validity = self.packed(MEASUREMENT + field)
        corners, weights = self.tie_support(window)
        valid = np.ones(window[2:], dtype=bool)
        value = np.zeros(window[2:])
        sine, cosine = np.zeros_like(value), np.zeros_like(value)
        for corner, weight in zip(corners, weights):
            valid &= validity[corner] | (weight == 0)
            value += np.where(weight > 0, values[corner] * weight, 0)
            if cyclic:
                sine += np.where(weight > 0, np.sin(np.deg2rad(values[corner])) * weight, 0)
                cosine += np.where(weight > 0, np.cos(np.deg2rad(values[corner])) * weight, 0)
        if cyclic:
            value = np.rad2deg(np.arctan2(sine, cosine)) % 360
        r, _ = self.indices(window)
        mask = 0x4f if field.startswith("solar_") else 0x47
        valid &= (self.all(QUALITY + "geolocation_quality").astype(np.uint32)[r // 24] & mask) == 0
        return value, valid

    def geometry(self, window):
        latitude, lat_valid = self.packed(MEASUREMENT + "latitude")
        longitude, lon_valid = self.packed(MEASUREMENT + "longitude")
        phi, lam = np.deg2rad(latitude), np.deg2rad(longitude)
        a, b = 6378137., 6356752.314245
        e2, ep2 = 1 - b ** 2 / a ** 2, (a ** 2 - b ** 2) / b ** 2
        n = a / np.sqrt(1 - e2 * np.sin(phi) ** 2)
        xyz = (n * np.cos(phi) * np.cos(lam), n * np.cos(phi) * np.sin(lam), n * (1 - e2) * np.sin(phi))
        corners, weights = self.tie_support(window)
        valid = np.ones(window[2:], dtype=bool)
        interpolated = [np.zeros(window[2:]) for _ in range(3)]
        for corner, weight in zip(corners, weights):
            valid &= (lat_valid[corner] & lon_valid[corner]) | (weight == 0)
            for axis in range(3):
                interpolated[axis] += np.where(weight > 0, xyz[axis][corner] * weight, 0)
        x, y, z = interpolated
        p = np.hypot(x, y)
        theta = np.arctan2(z * a, p * b)
        reconstructed_phi = np.arctan2(z + ep2 * b * np.sin(theta) ** 3, p - e2 * a * np.cos(theta) ** 3)
        row, col, rows, cols = window
        north, north_valid = self.packed(MEASUREMENT + "delta_lat_N_dem", (row, col), (rows, cols))
        east, east_valid = self.packed(MEASUREMENT + "delta_lon_E_dem", (row, col), (rows, cols))
        lat = np.rad2deg(reconstructed_phi + north / 6371000)
        lon = np.rad2deg(np.arctan2(y, x) + east / (6371000 * np.cos(reconstructed_phi)))
        lon = (lon + 180) % 360 - 180
        r, _ = self.indices(window)
        valid &= north_valid & east_valid
        valid &= (self.all(QUALITY + "geolocation_quality").astype(np.uint32)[r // 24] & 0x57) == 0
        valid &= np.isfinite(lat) & np.isfinite(lon) & (np.abs(lat) <= 90)
        return lat, lon, valid


def compare(actual, expected, valid, tolerance, name, cyclic=False):
    actual = np.array(actual, dtype=np.float64).reshape(expected.shape)
    if not np.all(np.isfinite(actual[valid])) or not np.all(np.isnan(actual[~valid])):
        raise AssertionError(name + ": finite/invalid values differ")
    delta = actual[valid] - expected[valid]
    if cyclic:
        delta = (delta + 180) % 360 - 180
    error = float(np.max(np.abs(delta), initial=0))
    if error > tolerance:
        raise AssertionError(f"{name}: error {error} exceeds {tolerance}")
    return error


def validate_reader(reader, path, oracle):
    rows, cols = oracle.shape
    windows = [(21, cols // 2 - 4, 6, 9), (0, 0, 2, 9), (rows - 2, cols - 9, 2, 9)]
    errors = {"reflectance": 0., "kelvin": 0., "latitude_degrees": 0., "longitude_degrees": 0., "angle_degrees": 0.}
    for window in windows:
        result = subprocess.run([reader, "--probe", path, *map(str, window)], text=True, capture_output=True, check=True, timeout=90)
        native = json.loads(result.stdout)
        for band in native["bands"]:
            identifier = band["id"]
            if identifier >= 0:
                values, valid = oracle.channel(identifier, window)
                key, tolerance = ("reflectance", 2e-5) if identifier < 11 else ("kelvin", .002)
            else:
                field = {-1001: "solar_zenith", -1002: "solar_azimuth", -1004: "observation_zenith", -1005: "observation_azimuth"}[identifier]
                values, valid = oracle.interpolate(field, window, "azimuth" in field)
                key, tolerance = "angle_degrees", 2e-5
            if not np.array_equal(np.array(band["valid"], dtype=bool).reshape(valid.shape), valid):
                raise AssertionError(f"{path.name}: band {identifier} validity differs")
            error = compare(band["values"], values, valid, tolerance, f"{path.name}: band {identifier}", identifier in (-1002, -1005))
            errors[key] = max(errors[key], error)
        lat, lon, valid = oracle.geometry(window)
        if not np.array_equal(np.array(native["geolocation_valid"], dtype=bool).reshape(valid.shape), valid):
            raise AssertionError(path.name + ": geolocation validity differs")
        for key, field, expected in (("latitude_degrees", "latitude", lat), ("longitude_degrees", "longitude", lon)):
            errors[key] = max(errors[key], compare(native[field], expected, valid, 2e-5, key, field == "longitude"))
    return {"channel_windows": len(windows) * 20, "geometry_windows": len(windows), "errors": errors}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--reader", type=Path, required=True)
    parser.add_argument("--data", type=Path, default=Path("testdata"))
    parser.add_argument("--output", type=Path, default=Path("output/metimage-validation"))
    parser.add_argument("--reader-only", action="store_true")
    args = parser.parse_args()
    app, reader, output = args.app.resolve(), args.reader.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records, groups = [], {}
    files = sorted(args.data.resolve().glob("W_*-VII-1B-RAD_*.zip"))
    if not files:
        raise RuntimeError("No METimage archives found")
    for path in files:
        oracle = NetcdfOracle(path)
        try:
            solar, valid = oracle.packed(MEASUREMENT + "solar_zenith")
            start, end = oracle.root_text("sensing_start_time_utc"), oracle.root_text("sensing_end_time_utc")
            record = {"file": path.name, "sha256": digest(path), "start": start, "end": end,
                      "shape": oracle.shape, "orbit": int(oracle.root_number("orbit_start")),
                      "night": bool(np.min(solar[valid]) >= 90), "sza_range": [float(np.min(solar[valid])), float(np.max(solar[valid]))],
                      "reader": validate_reader(reader, path, oracle)}
            records.append(record)
            groups.setdefault(record["orbit"], []).append((record, path))
            print(f"Reader passed: {path.name}", flush=True)
        finally:
            oracle.close()
    result = {"app_sha256": digest(app), "reader_sha256": digest(reader), "files": records,
              "channel_windows": sum(r["reader"]["channel_windows"] for r in records), "renders": []}
    (output / "reader-results.json").write_text(json.dumps(result, indent=2) + "\n")
    if args.reader_only:
        return

    def execute(command, name, mode, inputs):
        completed = subprocess.run(list(map(str, command)), text=True, capture_output=True, timeout=300)
        (output / (name + ".log")).write_text(completed.stdout + completed.stderr)
        if completed.returncode:
            raise RuntimeError(f"{name}: {completed.stdout}\n{completed.stderr}")
        destination = next(Path(command[i + 1]) for i, item in enumerate(command[:-1]) if str(item) == "-o") if "-o" in command else inputs["destination"]
        profile = json.loads(Path(str(destination) + ".profile.json").read_text())
        result["renders"].append({"name": name, "mode": mode, "inputs": inputs["files"],
                                  "wall_seconds": profile["wall_seconds"], "peak_rss_bytes": profile["process_peak_rss_bytes"],
                                  "source_pixels": profile["source_pixels"], "io": profile["io"],
                                  "output": profile["output"], "path": str(destination.relative_to(output)),
                                  "sha256": digest(destination)})
        (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
        print("Rendered:", name, flush=True)
        return destination

    def render(path, name, mode, band=0, extra=(), threads=4, width=768, extension="png"):
        destination = output / (name + "." + extension)
        command = [app, "render", "-d", path, "-m", mode, "-b", str(band), "--width", str(width),
                   "--threads", str(threads), "--profile", "-o", destination, *map(str, extra)]
        return execute(command, name, mode, {"files": [path.name], "destination": destination})

    def label(record):
        return record["start"][:19].replace("-", "").replace(" ", "-").replace(":", "")

    for record, path in zip(records, files):
        subprocess.run([app, "inspect", path], text=True, capture_output=True, check=True, timeout=60)
        render(path, label(record) + "-E17", "E", 17)
        render(path, label(record) + "-ECT", "ECT")
    day_record = next(r for r in records if "2026-10-22 00:56:58" in r["start"])
    night_record = next(r for r in records if "2026-10-20 12:49:58" in r["start"])
    day, night = args.data.resolve() / day_record["file"], args.data.resolve() / night_record["file"]
    for prefix, path in (("day", day), ("night", night)):
        for band in range(20):
            render(path, f"{prefix}-band-{band:02}", "R" if band < 11 else "E", band)
    for mode in ("RMUL", "RT", "RY", "RF", "EC", "ECT", "TSG"):
        render(day, "day-" + mode, mode)
    for mode in ("EC", "ECT"):
        render(night, "night-" + mode, mode)
    mosaic_images = []
    for orbit, entries in sorted(groups.items()):
        entries.sort(key=lambda e: e[0]["start"])
        modes = ("E", "EC", "ECT") if all(e[0]["night"] for e in entries) else ("R", "RMUL", "E", "RT", "RY", "RF", "EC", "ECT", "TSG")
        for mode in modes:
            name = f"orbit-{orbit}-{mode}"
            destination = output / (name + ".png")
            job = {"version": 1, "inputs": [{"data": str(p)} for _, p in reversed(entries)],
                   "mode": mode, "band": 17 if mode == "E" else 0,
                   "map": {"width": 768}, "output": str(destination)}
            job_path = output / (name + ".json")
            job_path.write_text(json.dumps(job, indent=2) + "\n")
            execute([app, "mosaic", "--job", job_path, "--threads", "4", "--profile"], name, mode,
                    {"files": [p.name for _, p in entries], "destination": destination})
            if mode in ("RT", "ECT"):
                mosaic_images.append(destination.name)
    crop = ("--center-lat", 35, "--center-lon", 135, "--half-range", 7, "--grid", "--grid-step", 2)
    for extension in ("png", "jpg", "tiff"):
        render(day, "crop-grid-" + extension, "RT", extra=crop, extension=extension)
    render(day, "orthographic-grid", "RT", extra=(*crop, "--projection", "orthographic"))
    for path, mode, prefix in ((day, "RT", "day"), (night, "ECT", "night")):
        hashes = []
        for threads in (1, 4, 0):
            destination = render(path, f"{prefix}-threads-{threads}", mode, threads=threads)
            hashes.append(digest(destination))
        if len(set(hashes)) != 1:
            raise AssertionError(prefix + ": thread outputs differ")
    render(day, "day-RT-width2400", "RT", width=2400)
    render(night, "night-ECT-width2400", "ECT", width=2400)
    result["mosaic_groups"] = {str(k): [p.name for _, p in entries] for k, entries in groups.items()}
    (output / "results.json").write_text(json.dumps(result, indent=2) + "\n")
    report = ["# METimage sample validation", "", f"{len(files)} files; {result['channel_windows']} independent channel windows; {len(result['renders'])} renders.", ""]
    for name in mosaic_images:
        report.extend([f"## {name}", "", f"![{name}]({name})", ""])
    (output / "README.md").write_text("\n".join(report))
    print(f"Passed: {len(result['renders'])} renders; {output / 'results.json'}", flush=True)


if __name__ == "__main__":
    main()
