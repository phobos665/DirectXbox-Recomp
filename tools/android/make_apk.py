#!/usr/bin/env python3
"""make_apk.py - wrap a title's libmain.so in an installable APK.

No Gradle: the shell is SDL's Java activity plus android/ in the toolkit,
compiled with javac and d8, linked with aapt2, aligned with zipalign and
signed with apksigner, all from the Android SDK's build tools. That is the
whole of what an APK needs here, and it is what a self-building release can
run on a player's machine with no network after the SDK is fetched.

    python3 tools/android/make_apk.py --lib build/android/libmain.so \\
        --lib <dxc>/libdxcompiler.so --package org.xboxrecomp.ts2 \\
        --label "TimeSplitters 2" --out ts2.apk

The signing key is made once and kept (--keystore, default
<per-user folder>/android/signing.p12): Android refuses an update signed with
another key, and replacing an app to change its key deletes its data, saves
included.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

TOOLKIT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SHELL = os.path.join(TOOLKIT, "android")
SDL_JAVA = os.path.join(TOOLKIT, "third_party", "SDL", "android-project", "app",
                        "src", "main", "java")
KEY_ALIAS = "xboxrecomp"
KEY_PASS = "xboxrecomp"   # protects nothing: the key lives in the player's own folder


def sdk_dir(arg):
    for d in (arg, os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"),
              os.path.expanduser("~/Library/Android/sdk"), os.path.expanduser("~/Android/Sdk")):
        if d and os.path.isdir(os.path.join(d, "platforms")):
            return d
    sys.exit("make_apk: no Android SDK (give --sdk or set ANDROID_HOME)")


def newest(pattern, what):
    found = sorted(glob.glob(pattern), key=lambda p: [int(x) if x.isdigit() else 0
                                                     for x in os.path.basename(p).replace("-", ".").split(".")])
    if not found:
        sys.exit("make_apk: no %s (%s)" % (what, pattern))
    return found[-1]


def run(cmd, **kw):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def default_keystore():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA", os.path.expanduser("~"))
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(os.environ.get("RECOMP_USER_DIR") or os.path.join(base, "xboxrecomp"),
                        "android", "signing.p12")


def ensure_key(path):
    if os.path.exists(path):
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print("make_apk: making the signing key %s (keep it: an update needs the same key)" % path)
    run(["keytool", "-genkeypair", "-keystore", path, "-storetype", "PKCS12",
         "-storepass", KEY_PASS, "-keypass", KEY_PASS, "-alias", KEY_ALIAS,
         "-keyalg", "RSA", "-keysize", "3072", "-validity", "36500",
         "-dname", "CN=xboxrecomp player build"])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--lib", action="append", required=True,
                    help="a shared library for lib/arm64-v8a (libmain.so first)")
    ap.add_argument("--package", required=True)
    ap.add_argument("--label", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--version-code", default="1")
    ap.add_argument("--version-name", default="1.0")
    ap.add_argument("--min-sdk", default="30")
    ap.add_argument("--target-sdk", default="34")
    ap.add_argument("--sdk")
    ap.add_argument("--keystore", default=None)
    ap.add_argument("--strip", action="store_true",
                    help="drop debug sections from the libraries (dynamic symbols stay)")
    a = ap.parse_args()

    sdk = sdk_dir(a.sdk)
    bt = newest(os.path.join(sdk, "build-tools", "*"), "build tools")
    platform = newest(os.path.join(sdk, "platforms", "android-*"), "platform")
    android_jar = os.path.join(platform, "android.jar")
    tool = lambda name: os.path.join(bt, name)
    keystore = a.keystore or default_keystore()

    work = tempfile.mkdtemp(prefix="xbr-apk-")
    try:
        # 1. Java: SDL's activity and ours, to one classes.dex.
        sources = glob.glob(os.path.join(SDL_JAVA, "org", "libsdl", "app", "*.java"))
        sources += glob.glob(os.path.join(SHELL, "java", "**", "*.java"), recursive=True)
        classes = os.path.join(work, "classes")
        os.makedirs(classes)
        run(["javac", "-nowarn", "-Xlint:none", "-encoding", "UTF-8", "--release", "11",
             "-classpath", android_jar, "-d", classes] + sources,
            stdout=subprocess.DEVNULL)
        class_files = glob.glob(os.path.join(classes, "**", "*.class"), recursive=True)
        dex = os.path.join(work, "dex")
        os.makedirs(dex)
        run([tool("d8"), "--release", "--min-api", a.min_sdk, "--lib", android_jar,
             "--output", dex] + class_files)

        # 2. The manifest, linked into a resource-only APK.
        with open(os.path.join(SHELL, "AndroidManifest.xml.in"), encoding="utf-8") as f:
            manifest = f.read()
        for k, v in {"PACKAGE": a.package, "LABEL": a.label, "VERSION_CODE": a.version_code,
                     "VERSION_NAME": a.version_name, "MIN_SDK": a.min_sdk,
                     "TARGET_SDK": a.target_sdk}.items():
            manifest = manifest.replace("@%s@" % k, v.replace("&", "&amp;").replace('"', "&quot;"))
        man_path = os.path.join(work, "AndroidManifest.xml")
        with open(man_path, "w", encoding="utf-8") as f:
            f.write(manifest)
        base = os.path.join(work, "base.apk")
        run([tool("aapt2"), "link", "-I", android_jar, "--manifest", man_path, "-o", base])

        # 3. Code and libraries in. Libraries stored, so they can be mapped
        # from the APK (extractNativeLibs=false) once zipalign pages them.
        unaligned = os.path.join(work, "unaligned.apk")
        shutil.copy(base, unaligned)
        with zipfile.ZipFile(unaligned, "a") as z:
            z.write(os.path.join(dex, "classes.dex"), "classes.dex",
                    compress_type=zipfile.ZIP_DEFLATED)
            for lib in a.lib:
                src = lib
                if a.strip:
                    strip = newest(os.path.join(sdk, "ndk", "*", "toolchains", "llvm",
                                                "prebuilt", "*", "bin", "llvm-strip"), "llvm-strip")
                    src = os.path.join(work, os.path.basename(lib))
                    run([strip, "--strip-debug", "-o", src, lib])
                z.write(src, "lib/arm64-v8a/" + os.path.basename(lib),
                        compress_type=zipfile.ZIP_STORED)

        # 4. Align (16 KB pages for the libraries), then sign.
        aligned = os.path.join(work, "aligned.apk")
        run([tool("zipalign"), "-P", "16", "-f", "4", unaligned, aligned])
        ensure_key(keystore)
        run([tool("apksigner"), "sign", "--ks", keystore, "--ks-pass", "pass:" + KEY_PASS,
             "--ks-key-alias", KEY_ALIAS, "--key-pass", "pass:" + KEY_PASS,
             "--out", a.out, aligned])
        print("make_apk: %s (%.1f MB)" % (a.out, os.path.getsize(a.out) / 1e6))
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
