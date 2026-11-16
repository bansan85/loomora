#!/usr/bin/env python3
"""Pick, boot and describe an iOS simulator of the newest iOS <major> runtime (macOS host only).

Used by .github/workflows/apple.yml (T126); it changes nothing but the state of one simulator device.

  python3 tools/ci/ios_simulator.py [--major 26]

Prints the runtimes and devices it saw, creates an iPhone device when the runtime has none, boots it
and waits for the end of the boot (`simctl bootstatus -b`). The last lines are

  udid=<UDID>
  platform_id=ios-<major.minor>-arm64-simulator     (the id the iOS Modern UI puts in its probe line)

and, when $GITHUB_OUTPUT is set, the same two lines are appended to it. Exit status: 0 when a
simulator is booted, 1 otherwise (the reason is printed).
"""
import argparse
import json
import os
import subprocess
import sys


def simctl(*args, timeout=300):
    print("$ xcrun simctl " + " ".join(args), flush=True)
    res = subprocess.run(["xcrun", "simctl"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         timeout=timeout)
    return res.returncode, res.stdout.decode("utf-8", "replace"), res.stderr.decode("utf-8", "replace")


def version_key(text):
    return tuple(int(part) for part in text.split(".") if part.isdigit())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--major", type=int, default=26, help="iOS major version of the runtime (default 26)")
    args = ap.parse_args()

    code, out, err = simctl("list", "-j", "runtimes")
    if code != 0:
        print("simctl list runtimes failed: %s %s" % (out, err))
        return 1
    runtimes = [r for r in json.loads(out)["runtimes"]
                if r.get("platform", "iOS") == "iOS" and r.get("isAvailable", True)
                and version_key(r["version"])[:1] == (args.major,)]
    print("iOS %d runtimes: %s" % (args.major, ", ".join("%s (%s)" % (r["version"], r["identifier"])
                                                          for r in runtimes) or "none"))
    if not runtimes:
        print("no available iOS %d simulator runtime on this runner image" % args.major)
        simctl("list", "runtimes")
        return 1
    runtime = max(runtimes, key=lambda r: version_key(r["version"]))

    code, out, err = simctl("list", "-j", "devices", "available")
    devices = json.loads(out)["devices"].get(runtime["identifier"], []) if code == 0 else []
    iphones = [d for d in devices if d["name"].startswith("iPhone")]
    print("iPhone devices of %s: %s" % (runtime["identifier"], ", ".join(d["name"] for d in iphones) or "none"))
    if iphones:
        device = iphones[0]
    else:
        code, out, err = simctl("list", "-j", "devicetypes")
        types = [t for t in json.loads(out)["devicetypes"] if "iPhone" in t["identifier"]] if code == 0 else []
        if not types:
            print("no iPhone device type to create a simulator from")
            return 1
        code, out, err = simctl("create", "loomora-iphone", types[-1]["identifier"], runtime["identifier"])
        if code != 0:
            print("simctl create failed: %s %s" % (out, err))
            return 1
        device = {"udid": out.strip(), "name": "loomora-iphone"}
    udid = device["udid"]
    print("simulator: %s (udid %s), runtime iOS %s" % (device["name"], udid, runtime["version"]))

    # bootstatus -b boots the device when it is shut down and waits until the system is ready
    code, out, err = simctl("bootstatus", udid, "-b", timeout=900)
    print(out + err)
    if code != 0:
        print("simctl bootstatus -b failed (exit %d)" % code)
        return 1
    code, out, err = simctl("list", "devices", "booted")
    print(out + err)

    version = version_key(runtime["version"])
    platform_id = "ios-%d.%d-arm64-simulator" % (version[0], version[1] if len(version) > 1 else 0)
    lines = ["udid=" + udid, "platform_id=" + platform_id]
    for line in lines:
        print(line)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line in lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
