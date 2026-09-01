# PlatformIO extra script (pre:) — firmware version stamping + release folder.
#
# 1. On interactive builds, prompts for the firmware version — plain Enter
#    keeps the current one. config.h's FW_VERSION stays the single source of
#    truth: a new version is written back there (pre-compile, so the binary,
#    the device's reported version, and the release filename all agree).
#    Set FW_VERSION_OVERRIDE=x.y.z for a non-interactive bump.
# 2. Injects FW_GIT_REV (short git commit, "-dirty" if uncommitted changes)
#    as a compile-time define so the device can report exactly what it runs.
# 3. After each build, copies into firmware/:
#      stackedbitcoin-pos_v<FW_VERSION>_<YYYY-MM-DD>_<commit>.bin          (OTA app image)
#      stackedbitcoin-pos_v<FW_VERSION>_<YYYY-MM-DD>_<commit>_factory.bin (full image @0x0)
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
import sys

PROJ = env["PROJECT_DIR"]
OUTDIR = os.path.join(PROJ, "firmware")

# Old lightningpay-pos builds stay matched so they remain in the manifest.
NAME_RE = re.compile(
    r"^(?:stackedbitcoin|lightningpay)-pos_v(?P<ver>[^_]+)_(?P<date>\d{4}-\d{2}-\d{2})"
    r"_(?P<rev>[0-9a-fA-F]+(?:-dirty)?)(?P<fact>_factory)?\.bin$")


def sh(cmd):
    try:
        return subprocess.check_output(
            cmd, cwd=PROJ, stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


def fw_version():
    with open(os.path.join(PROJ, "config.h"), encoding="utf-8") as f:
        m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
    return m.group(1) if m else "0.0.0"


# x.y.z with optional [.-]suffix. Never '_' — NAME_RE parses version as [^_]+.
VER_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.]+)?$")

# Targets that compile nothing — don't prompt for those. Cleans never reach
# COMMAND_LINE_TARGETS (PIO passes --clean instead), hence the GetOption
# check in resolve_version(). __idedata/__debug are the real target names
# behind `pio project metadata` / `pio check` / `pio debug` / IDE refreshes,
# which swallow stdout entirely — prompting there would hang invisibly.
SKIP_TARGETS = {"erase", "monitor", "envdump", "__idedata", "__debug"}
try:
    from SCons.Script import COMMAND_LINE_TARGETS
except ImportError:
    COMMAND_LINE_TARGETS = []


def valid_version(v):
    return bool(VER_RE.match(v)) and "_" not in v


def resolve_version():
    cur = fw_version()
    ov = os.environ.get("FW_VERSION_OVERRIDE")
    if ov:  # scripted releases — no prompt
        if valid_version(ov):
            return ov
        print("fw_version: WARNING ignoring invalid FW_VERSION_OVERRIDE %r" % ov)
        return cur
    if env.GetOption("clean") or SKIP_TARGETS.intersection(COMMAND_LINE_TARGETS):
        return cur
    if os.environ.get("CI") or not (sys.stdin and sys.stdin.isatty()):
        return cur
    # PIO forwards SCons stdout through a line-buffered pipe, so a bare
    # input() prompt (no trailing newline) never appears and the build just
    # hangs. Talk to the controlling terminal directly instead. Two opens —
    # buffered "r+" refuses non-seekable streams like a tty.
    try:
        tin = open("/dev/tty", "r")
        tout = open("/dev/tty", "w")
    except OSError:
        return cur  # no controlling terminal after all
    with tin, tout:
        for _ in range(3):  # KeyboardInterrupt propagates — aborts the build
            tout.write("fw_version: firmware version [%s]: " % cur)
            tout.flush()
            line = tin.readline()
            if not line:  # EOF
                return cur
            v = line.strip()
            if not v:
                return cur
            if valid_version(v):
                return v
            tout.write("fw_version: invalid %r (want x.y.z, optional [.-]suffix, no '_')\n" % v)
        tout.write("fw_version: WARNING 3 invalid attempts, keeping %s\n" % cur)
    return cur


def store_version(ver):
    # Rewrite only the FW_VERSION define; everything else stays byte-for-byte.
    # Temp-file + rename so an interrupt can't leave config.h truncated.
    path = os.path.join(PROJ, "config.h")
    with open(path, newline="", encoding="utf-8") as f:
        text = f.read()
    new = re.sub(r'(#define\s+FW_VERSION\s+")[^"]*(")',
                 lambda m: m.group(1) + ver + m.group(2), text, count=1)
    if new != text:
        tmp = path + ".tmp"
        with open(tmp, "w", newline="", encoding="utf-8") as f:
            f.write(new)
        os.replace(tmp, path)
        print("fw_version: config.h FW_VERSION -> %s" % ver)


FW_VER = resolve_version()
store_version(FW_VER)
print("fw_version: building v%s" % FW_VER)

# After store_version — a version bump dirties the tree, and the build must
# stamp itself accordingly (the commit alone can't reproduce it).
REV = sh(["git", "rev-parse", "--short", "HEAD"]) or "nogit"
if sh(["git", "status", "--porcelain"]):
    REV += "-dirty"

env.Append(CPPDEFINES=[("FW_GIT_REV", '\\"%s\\"' % REV)])


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

    # Highest version first (the web flasher badges builds[0] as LATEST);
    # mtime breaks ties so rebuilding an old version can't jump the queue.
    def ver_key(v):
        m = re.match(r"(\d+)\.(\d+)\.(\d+)", v)
        return tuple(int(x) for x in m.groups()) if m else (0, 0, 0)

    ordered = sorted(builds.values(),
                     key=lambda b: (ver_key(b["version"]), b["_mtime"]),
                     reverse=True)
    for b in ordered:
        del b["_mtime"]

    manifest = {"name": "Stacked Bitcoin POS", "chip": "ESP32-P4", "builds": ordered}
    with open(os.path.join(OUTDIR, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    print("fw_version: manifest.json — %d build(s)" % len(ordered))


def copy_release(source, target, env):
    stamp = datetime.date.today().isoformat()
    stem = "stackedbitcoin-pos_v%s_%s_%s" % (FW_VER or fw_version(), stamp, REV)
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
