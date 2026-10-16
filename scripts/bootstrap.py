#!/usr/bin/env python3
"""Install locked dependencies to a host-local prefix, without sudo."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / ".deps"


def run(*command, env=None, cwd=ROOT):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), env=env, cwd=cwd, check=True)


def verify(path, checksum):
    return path.is_file() and hashlib.file_digest(path.open("rb"), "sha256").hexdigest() == checksum


def download(url, path, checksum):
    if not verify(path, checksum):
        temporary = path.with_suffix(".download")
        urllib.request.urlretrieve(url, temporary)
        if not verify(temporary, checksum):
            temporary.unlink()
            raise RuntimeError(f"SHA256 mismatch: {url}")
        temporary.replace(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="Verify cached downloads without installing")
    parser.add_argument("--jobs", type=int, default=8)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("jobs must be positive")
    lock = json.loads((ROOT / "dependencies.lock.json").read_text())
    if args.check:
        files = [(DEPS / "src" / Path(a["url"]).name, a["sha256"]) for a in lock["gstreamer"]]
        files += [(DEPS / "packages" / p["filename"], p["sha256"]) for p in lock["ubuntu"]["packages"]]
        files += [(DEPS / "packages" / Path(p["Filename"]).name, p["SHA256"]) for p in lock["rocm"]["packages"]]
        missing = [str(path) for path, checksum in files if not verify(path, checksum)]
        print(json.dumps({"passed": not missing, "checked": len(files), "invalid": missing}))
        return 2 if missing else 0
    for tool in ("uv", "cmake", "pkg-config", "git", "dpkg-deb", "apt-get", "m4", "cc", "c++"):
        if not shutil.which(tool):
            raise RuntimeError(f"Required host tool not installed: {tool}")
    for directory in ("src", "packages", "sysroot", "prefix", "pkgconfig"):
        (DEPS / directory).mkdir(parents=True, exist_ok=True)
    if not (DEPS / "venv/bin/python").exists():
        run("uv", "venv", DEPS / "venv")
    run("uv", "pip", "install", "--python", DEPS / "venv/bin/python",
        *(f"{name}=={version}" for name, version in lock["python"].items()))
    for package in lock["ubuntu"]["packages"]:
        path = DEPS / "packages" / package["filename"]
        if not verify(path, package["sha256"]):
            run("apt-get", "download", f"{package['name']}={package['version']}", cwd=path.parent)
        if not verify(path, package["sha256"]):
            raise RuntimeError(f"Ubuntu package checksum mismatch: {path}")
        run("dpkg-deb", "-x", path, DEPS / "sysroot")
    for package in lock["rocm"]["packages"]:
        path = DEPS / "packages" / Path(package["Filename"]).name
        download(lock["rocm"]["repository"] + package["Filename"], path, package["SHA256"])
        run("dpkg-deb", "-x", path, DEPS / "rocm72")
    for name in ("libva", "libva-drm", "json-glib-1.0", "sqlite3", "libdrm", "libdrm_amdgpu",
                 "fontconfig", "freetype2", "libpng", "libpng16", "zlib",
                 "libbrotlicommon", "libbrotlidec", "libbrotlienc", "expat"):
        path = DEPS / "sysroot/usr/lib/x86_64-linux-gnu/pkgconfig" / f"{name}.pc"
        (DEPS / "pkgconfig" / path.name).write_text(path.read_text().replace("prefix=/usr", f"prefix={DEPS}/sysroot/usr"))
    source = DEPS / "src/dlstreamer"
    if not source.exists():
        run("git", "clone", "--depth", "1", "--branch", lock["dlstreamer"]["tag"], lock["dlstreamer"]["url"], source)
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != lock["dlstreamer"]["commit"]:
        raise RuntimeError("DL Streamer checkout differs from lock")
    patch = source / lock["dlstreamer"]["analytics_patch"]
    if not verify(patch, lock["dlstreamer"]["analytics_patch_sha256"]):
        raise RuntimeError("DL Streamer analytics patch differs from lock")
    for name in ("dlstreamer-gstanalytics-label-id.patch", "dlstreamer-amd-metadata-merge.patch", "dlstreamer-stream-identities.patch"):
        local_patch = ROOT / "patches" / name
        if subprocess.run(["git", "apply", "--reverse", "--check", str(local_patch)], cwd=source, capture_output=True).returncode:
            run("git", "apply", local_patch, cwd=source)
    for archive in lock["gstreamer"]:
        path = DEPS / "src" / Path(archive["url"]).name
        download(archive["url"], path, archive["sha256"])
        if not (DEPS / "src" / f"{archive['name']}-{archive['version']}").exists():
            with tarfile.open(path) as tar:
                tar.extractall(DEPS / "src", filter="data")
    bad = DEPS / "src/gst-plugins-bad-1.28.2"
    if not (bad / "gst-libs/gst/analytics/gstanalyticskeypointmtd.h").exists():
        run("git", "apply", "-p3", "--exclude=*/GstAnalytics.py", f"--directory={bad.relative_to(ROOT)}", patch)
    # Read the same environment that developers use; bash preserves exact paths.
    raw = subprocess.check_output(["bash", "-c", "source scripts/env.sh; env -0"], cwd=ROOT)
    env = {item.split(b"=", 1)[0].decode(): item.split(b"=", 1)[1].decode()
           for item in raw.split(b"\0") if b"=" in item}
    variants = {
        "gstreamer": ["-Dtests=disabled", "-Dexamples=disabled", "-Dintrospection=disabled", "-Ddoc=disabled", "-Dbenchmarks=disabled", "-Dcheck=disabled", "-Dlibdw=disabled", "-Dlibunwind=disabled", "-Dptp-helper=disabled"],
        "gst-plugins-base": ["-Dauto_features=disabled", "-Dapp=enabled", "-Dvideoconvertscale=enabled", "-Dvideotestsrc=enabled", "-Dtypefind=enabled", "-Dplayback=enabled", "-Dgio=enabled", "-Dtcp=enabled"],
        "gst-plugins-bad": ["-Dauto_features=disabled", "-Dva=enabled", "-Dvideoparsers=enabled"],
        "gst-plugins-good": ["-Dauto_features=disabled", "-Dmultifile=enabled", "-Drtp=enabled", "-Drtpmanager=enabled", "-Drtsp=enabled", "-Dudp=enabled", "-Dmatroska=enabled", "-Disomp4=enabled"],
    }
    variants["gst-rtsp-server"] = ["-Dauto_features=disabled", "-Dtests=disabled", "-Dexamples=disabled", "-Dintrospection=disabled"]
    for name, options in variants.items():
        build = DEPS / f"locked-build-{name}"
        command = ["meson", "setup"]
        if (build / "build.ninja").exists():
            command += ["--reconfigure"]
        run(*command, build, DEPS / "src" / f"{name}-1.28.2", f"--prefix={DEPS}/prefix", "--libdir=lib", *options, env=env)
        run("meson", "compile", "-C", build, "-j", args.jobs, env=env)
        run("meson", "install", "-C", build, env=env)
    openvino = next((DEPS / "venv/lib").glob("python*/site-packages/openvino/cmake"))
    run("cmake", "-S", ROOT / "cmake/dlstreamer-selected", "-B", DEPS / "build-dls",
        f"-DDLS_SOURCE={source}", f"-DSYSROOT={DEPS}/sysroot", f"-DOpenVINO_DIR={openvino}",
        f"-DCMAKE_INSTALL_PREFIX={DEPS}/prefix", "-DCMAKE_BUILD_TYPE=Release", env=env)
    run("cmake", "--build", DEPS / "build-dls", "-j", args.jobs, env=env)
    run("cmake", "--install", DEPS / "build-dls", env=env)
    run("cmake", "-S", ROOT, "-B", ROOT / "build", f"-DAMDVA_ROCM_ROOT={env['AMDVA_ROCM_ROOT']}",
        f"-DCMAKE_PREFIX_PATH={DEPS}/sysroot/usr", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DAMDVA_DLSTREAMER_TESTS=ON", f"-DCMAKE_INSTALL_PREFIX={DEPS}/prefix", env=env)
    run("cmake", "--build", ROOT / "build", "-j", args.jobs, env=env)
    run("ctest", "--test-dir", ROOT / "build", "--output-on-failure", env=env)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
