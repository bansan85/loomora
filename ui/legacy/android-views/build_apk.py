#!/usr/bin/env python
"""Package the Android Legacy UI: the scripted APK build that replaces Gradle (T081).

Usage:
  python ui/legacy/android-views/build_apk.py --out OUT.apk --workdir DIR --lib ABI=libloomora_jni.so
                                              [--lib ...] [--keystore FILE]

Steps (all command-line tools of the Android SDK, no Gradle, no AndroidX):
  1. javac (JDK 8 or later) compiles src/ against platforms/android-9/android.jar with
     -source 8 -target 8 (the oldest level a current JDK accepts; there is no Java 7 output to ask
     for). Only the API 9 classes are visible, so a newer framework call does not compile.
  2. d8 (R8, build-tools 36.0.0) converts the classes to classes.dex with --min-api 9 (it also
     desugars what Java 8 needs; the code uses no Java 7+ feature anyway). No dx: build-tools 36
     does not ship it, d8 accepts --min-api 9.
  3. aapt packages AndroidManifest.xml (minSdkVersion 9); there are no resources.
  4. classes.dex and lib/<abi>/libloomora_jni.so (stripped) are added with zipfile.
  5. zipalign -p 4, then apksigner with the v1 (JAR) scheme ONLY: API 9 to 23 do not know v2, and
     apksigner picks SHA-1 digests for a minimum SDK below 18. The key is a debug key generated
     once in --keystore (keytool), nothing secret.

Environment (no global variable is changed, paths are only read):
  LOOMORA_ANDROID_SDK (or ANDROID_HOME / ANDROID_SDK_ROOT)  the Android SDK, with platforms/android-9
                                                              and build-tools (newest, or
                                                              LOOMORA_ANDROID_BUILD_TOOLS=<version>)
  JAVA_HOME or javac / keytool on PATH
"""
import argparse
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MIN_SDK = 9
ANDROID_PLATFORM = "android-9"


def fail(message):
    sys.exit("build_apk: " + message)


def sdk_dir():
    for name in ("LOOMORA_ANDROID_SDK", "ANDROID_HOME", "ANDROID_SDK_ROOT"):
        if os.environ.get(name):
            return Path(os.environ[name])
    fail("set LOOMORA_ANDROID_SDK (or ANDROID_HOME) to the Android SDK directory")


def build_tools(sdk):
    root = sdk / "build-tools"
    wanted = os.environ.get("LOOMORA_ANDROID_BUILD_TOOLS")
    if wanted:
        return root / wanted
    versions = sorted((d for d in root.iterdir() if d.is_dir()),
                      key=lambda d: [int(p) if p.isdigit() else 0 for p in d.name.split(".")]) \
        if root.is_dir() else []
    if not versions:
        fail("no build-tools in %s (sdkmanager \"build-tools/36.0.0\")" % root)
    return versions[-1]


def java_tool(name):
    home = os.environ.get("JAVA_HOME")
    if home:
        for candidate in (Path(home) / "bin" / name, Path(home) / "bin" / (name + ".exe")):
            if candidate.is_file():
                return str(candidate)
    found = shutil.which(name)
    if not found:
        fail("%s not found: set JAVA_HOME or put a JDK on PATH" % name)
    return found


def tool(directory, name):
    """A build-tools executable: name.exe / name.bat on Windows, name elsewhere."""
    for suffix in (".exe", ".bat", ""):
        candidate = directory / (name + suffix)
        if candidate.is_file():
            return str(candidate)
    fail("%s not found in %s" % (name, directory))


def run(cmd, **kwargs):
    res = subprocess.run([str(c) for c in cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, **kwargs)
    if res.returncode != 0:
        sys.stderr.write(res.stdout)
        fail("command failed (%d): %s" % (res.returncode, " ".join(str(c) for c in cmd[:3])))
    return res.stdout


def main(argv=None):
    ap = argparse.ArgumentParser(description="Package the Android Legacy UI as an APK.")
    ap.add_argument("--out", required=True, help="the APK to write")
    ap.add_argument("--workdir", required=True, help="scratch directory (classes, dex, keystore)")
    ap.add_argument("--lib", action="append", default=[], metavar="ABI=FILE",
                    help="a libloomora_jni.so of one ABI (armeabi, armeabi-v7a, x86, ...)")
    ap.add_argument("--strip", action="append", default=[], metavar="ABI=TOOL",
                    help="the strip executable of an ABI (the library is stripped when given)")
    ap.add_argument("--keystore", help="debug keystore (default: WORKDIR/debug.keystore)")
    args = ap.parse_args(argv)
    if not args.lib:
        fail("give at least one --lib ABI=FILE")

    sdk = sdk_dir()
    bt = build_tools(sdk)
    android_jar = sdk / "platforms" / ANDROID_PLATFORM / "android.jar"
    if not android_jar.is_file():
        fail("%s is missing (sdkmanager \"platforms;android-9\")" % android_jar)
    work = Path(args.workdir)
    if work.exists():
        shutil.rmtree(str(work))
    classes = work / "classes"
    classes.mkdir(parents=True)

    sources = sorted((HERE / "src").rglob("*.java"))
    run([java_tool("javac"), "-source", "8", "-target", "8", "-Xlint:-options", "-encoding", "UTF-8",
         "-bootclasspath", android_jar, "-d", classes] + sources)
    class_files = sorted(classes.rglob("*.class"))
    d8_dir = work / "dex"
    d8_dir.mkdir()
    run([tool(bt, "d8"), "--min-api", str(MIN_SDK), "--lib", android_jar, "--output", d8_dir]
        + class_files)
    dex = d8_dir / "classes.dex"
    if not dex.is_file():
        fail("d8 produced no classes.dex")
    version = dex.read_bytes()[:8]
    print("classes.dex: %d bytes, header %r" % (dex.stat().st_size, version))

    base = work / "base.apk"
    run([tool(bt, "aapt"), "package", "-f", "-M", HERE / "AndroidManifest.xml", "-I", android_jar,
         "-F", base])

    strip = dict(item.split("=", 1) for item in args.strip)
    unaligned = work / "unaligned.apk"
    shutil.copyfile(str(base), str(unaligned))
    with zipfile.ZipFile(str(unaligned), "a", zipfile.ZIP_DEFLATED) as apk:
        apk.write(str(dex), "classes.dex")
        for item in args.lib:
            abi, path = item.split("=", 1)
            lib = Path(path)
            if abi in strip:
                stripped = work / ("libloomora_jni-%s.so" % abi)
                shutil.copyfile(str(lib), str(stripped))
                run([strip[abi], "--strip-unneeded", stripped])
                lib = stripped
            apk.write(str(lib), "lib/%s/libloomora_jni.so" % abi)

    aligned = work / "aligned.apk"
    run([tool(bt, "zipalign"), "-f", "-p", "4", unaligned, aligned])

    keystore = Path(args.keystore) if args.keystore else work / "debug.keystore"
    if not keystore.is_file():
        keystore.parent.mkdir(parents=True, exist_ok=True)
        run([java_tool("keytool"), "-genkeypair", "-keystore", keystore, "-storepass", "android",
             "-keypass", "android", "-alias", "loomora-debug", "-keyalg", "RSA", "-keysize", "2048",
             "-validity", "10000", "-dname", "CN=Loomora debug,O=Loomora,C=FR"])
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    run([tool(bt, "apksigner"), "sign", "--ks", keystore, "--ks-pass", "pass:android",
         "--key-pass", "pass:android", "--ks-key-alias", "loomora-debug",
         "--min-sdk-version", str(MIN_SDK),
         "--v1-signing-enabled", "true", "--v2-signing-enabled", "false",
         "--v3-signing-enabled", "false", "--out", out, aligned])
    print(run([tool(bt, "apksigner"), "verify", "--verbose", "--min-sdk-version", str(MIN_SDK),
               out]).strip())
    print("apk: %s (%d bytes)" % (out, out.stat().st_size))
    return 0


if __name__ == "__main__":
    sys.exit(main())
