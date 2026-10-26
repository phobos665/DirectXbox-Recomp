#!/usr/bin/env python3
"""build_apk.py - a title's Android APK, from its already-lifted project.

    python3 xboxrecomp/tools/android/build_apk.py --package org.xboxrecomp.ts2 \\
        --label "TimeSplitters 2" [--project .] [--out build/android/<name>.apk]

The project is a title repository (its CMakeLists.txt pulls the toolkit in)
whose game has been lifted for the desktop already (src/recomp/gen): the
lifted C is the same on every host. This configures build/android with the
NDK's toolchain (arm64-v8a, API 30), builds libmain.so, and wraps it with
libdxcompiler.so in an APK (make_apk.py), told the SHA-1 of the project's
game/default.xbe so the app looks for that game on the device.

The launcher icon is --icon, else the one the title's release recipe names
(<project>/release/*.json, or --recipe), else the toolkit's own:

    "android": {"icon": "release/android/icon.png"}

The path is relative to the project, as the recipe's image configs are. A PNG,
square, 192 pixels or more; make_apk.py scales it to every density.

DXC for Android comes from tools/android/build_dxc.sh: --dxc names the .so,
else <toolkit>/third_party/dxc-android/lib/libdxcompiler.so.
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLKIT = os.path.abspath(os.path.join(HERE, "..", ".."))


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def find_sdk(arg):
    for d in (arg, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
              os.path.expanduser("~/Library/Android/sdk"), os.path.expanduser("~/Android/Sdk")):
        if d and os.path.isdir(d):
            return d
    sys.exit("build_apk: no Android SDK (give --sdk or set ANDROID_HOME)")


def find_ndk(arg, sdk):
    if arg:
        return arg
    if os.environ.get("ANDROID_NDK_HOME"):
        return os.environ["ANDROID_NDK_HOME"]
    # The newest release NDK (not a beta): its source.properties has no "beta".
    best = None
    for d in glob.glob(os.path.join(sdk, "ndk", "*")):
        props = os.path.join(d, "source.properties")
        if not os.path.isfile(props) or "beta" in open(props).read():
            continue
        ver = [int(x) for x in os.path.basename(d).split(".") if x.isdigit()]
        if best is None or ver > best[0]:
            best = (ver, d)
    if not best:
        sys.exit("build_apk: no NDK under %s/ndk (give --ndk or set ANDROID_NDK_HOME)" % sdk)
    return best[1]


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def recipe_icon(project, recipe):
    """The icon the title's release recipe names under "android", or None.
    With no --recipe, the one recipe in <project>/release, if there is one."""
    if not recipe:
        found = sorted(glob.glob(os.path.join(project, "release", "*.json")))
        if len(found) != 1:
            return None
        recipe = found[0]
    with open(recipe, encoding="utf-8") as f:
        icon = (json.load(f).get("android") or {}).get("icon")
    if not icon:
        return None
    path = os.path.join(project, icon)
    if not os.path.isfile(path):
        sys.exit("build_apk: %s names the icon %s, which is not there" % (recipe, path))
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--project", default=".")
    ap.add_argument("--package", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out")
    ap.add_argument("--xbe", help="the default.xbe that was lifted (default <project>/game/default.xbe)")
    ap.add_argument("--icon", help="the launcher icon, a PNG (default: the recipe's "
                                   "android.icon, else the toolkit's)")
    ap.add_argument("--recipe", help="the release recipe to read android.icon from "
                                     "(default: the one in <project>/release)")
    ap.add_argument("--dxc")
    ap.add_argument("--sdk")
    ap.add_argument("--ndk")
    ap.add_argument("--api", default="30")
    ap.add_argument("--version-code", default="1")
    ap.add_argument("--version-name", default="1.0")
    a = ap.parse_args()

    project = os.path.abspath(a.project)
    build = os.path.join(project, "build", "android")
    sdk = find_sdk(a.sdk)
    ndk = find_ndk(a.ndk, sdk)
    dxc = a.dxc or os.path.join(TOOLKIT, "third_party", "dxc-android", "lib", "libdxcompiler.so")
    if not os.path.isfile(dxc):
        sys.exit("build_apk: no DXC for Android at %s (tools/android/build_dxc.sh makes one)" % dxc)
    xbe = a.xbe or os.path.join(project, "game", "default.xbe")
    if not os.path.isfile(xbe):
        sys.exit("build_apk: no %s: the APK is told which game to look for from it" % xbe)
    icon = a.icon or recipe_icon(project, a.recipe)

    if not os.path.isfile(os.path.join(build, "build.ninja")):
        run(["cmake", "-S", project, "-B", build, "-G", "Ninja",
             "-DCMAKE_TOOLCHAIN_FILE=" + os.path.join(ndk, "build", "cmake", "android.toolchain.cmake"),
             "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-" + a.api,
             "-DANDROID_STL=c++_static", "-DCMAKE_BUILD_TYPE=Release"])
    run(["cmake", "--build", build])
    lib = os.path.join(build, "libmain.so")
    if not os.path.isfile(lib):
        sys.exit("build_apk: the build made no libmain.so: does the title's CMakeLists "
                 "build a SHARED library named main on Android?")

    out = a.out or os.path.join(build, a.package + ".apk")
    env = dict(os.environ, ANDROID_HOME=sdk)
    run([sys.executable, os.path.join(HERE, "make_apk.py"), "--lib", lib, "--lib", dxc,
         "--package", a.package, "--label", a.label, "--xbe-sha1", sha1_of(xbe),
         "--min-sdk", a.api, "--version-code", a.version_code,
         "--version-name", a.version_name, "--strip", "--out", out] +
        (["--icon", icon] if icon else []), env=env)


if __name__ == "__main__":
    main()
