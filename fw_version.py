# PlatformIO extra script (pre:) — firmware version stamping + release folder.
#
# 1. Injects FW_GIT_REV (short git commit, "-dirty" if uncommitted changes)
#    as a compile-time define so the device can report exactly what it runs.
# 2. After each build, copies into firmware/:
#      lightningpay-pos_v<FW_VERSION>_<YYYY-MM-DD>_<commit>.bin          (OTA app image)
#      lightningpay-pos_v<FW_VERSION>_<YYYY-MM-DD>_<commit>_factory.bin (full image @0x0)
#    and regenerates firmware/manifest.json describing every build in the
#    folder (version/date/commit/size/md5/sha256, newest first).
#
# Publish the firmware/ folder to the update site: the web flasher page,
# the on-device update menu, and the local /update portal all feed off it.
Import("env")

import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess

PROJ = env["PROJECT_DIR"]
OUTDIR = os.path.join(PROJ, "firmware")

NAME_RE = re.compile(
    r"^lightningpay-pos_v(?P<ver>[^_]+)_(?P<date>\d{4}-\d{2}-\d{2})"
    r"_(?P<rev>[0-9a-fA-F]+(?:-dirty)?)(?P<fact>_factory)?\.bin$")


def sh(cmd):
    try:
        return subprocess.check_output(
            cmd, cwd=PROJ, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


REV = sh(["git", "rev-parse", "--short", "HEAD"]) or "nogit"
if sh(["git", "status", "--porcelain"]):
    REV += "-dirty"

env.Append(CPPDEFINES=[("FW_GIT_REV", '\\"%s\\"' % REV)])


def fw_version():
    with open(os.path.join(PROJ, "config.h")) as f:
        m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
    return m.group(1) if m else "0.0.0"


def gen_manifest():
    builds = {}
    for fn in sorted(os.listdir(OUTDIR)):
        m = NAME_RE.match(fn)
        if not m:
            continue
        path = os.path.join(OUTDIR, fn)
        data = open(path, "rb").read()
        entry = {
            "file": fn,
            "size": len(data),
            "md5": hashlib.md5(data).hexdigest(),
            "sha256": hashlib.sha256(data).hexdigest(),
        }
        key = (m["ver"], m["date"], m["rev"])
        b = builds.setdefault(key, {
            "version": m["ver"],
            "date": m["date"],
            "commit": m["rev"],
            "_mtime": 0,
        })
        b["factory" if m["fact"] else "app"] = entry
        b["_mtime"] = max(b["_mtime"], os.path.getmtime(path))

    ordered = sorted(builds.values(), key=lambda b: b["_mtime"], reverse=True)
    for b in ordered:
        del b["_mtime"]

    manifest = {"name": "Lightning Pay POS", "chip": "ESP32-P4", "builds": ordered}
    with open(os.path.join(OUTDIR, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("fw_version: manifest.json — %d build(s)" % len(ordered))


def copy_release(source, target, env):
    stamp = datetime.date.today().isoformat()
    stem = "lightningpay-pos_v%s_%s_%s" % (fw_version(), stamp, REV)
    os.makedirs(OUTDIR, exist_ok=True)

    app = env.subst("$BUILD_DIR/${PROGNAME}.bin")
    shutil.copy2(app, os.path.join(OUTDIR, stem + ".bin"))
    print("fw_version: copied build to firmware/%s.bin" % stem)

    # Full image (bootloader+partitions+otadata+app, flash at 0x0) for
    # USB-flashing blank or bricked boards from the web flasher.
    factory = env.subst("$BUILD_DIR/${PROGNAME}.factory.bin")
    if os.path.exists(factory):
        shutil.copy2(factory, os.path.join(OUTDIR, stem + "_factory.bin"))
        print("fw_version: copied factory image to firmware/%s_factory.bin" % stem)

    gen_manifest()


# "buildprog" runs after the whole program build — including the platform's
# firmware.factory.bin merge step, which a post-action on firmware.bin
# would race against.
env.AddPostAction("buildprog", copy_release)
