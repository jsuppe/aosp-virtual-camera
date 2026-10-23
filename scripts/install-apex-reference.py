#!/usr/bin/env python3
"""Stage a signed virtual-camera APEX on the already-integrated A13 reference VM.

No root, remount, signing bypass, policy edits or downgrade flags. Reboot only
with --reboot. Activation verification is NOT camera/release qualification.
Requires Python 3.9+, adb and aapt. Physical devices deliberately unsupported.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

PACKAGE = "com.android.hardware.camera.provider.virtual"
REFERENCE = "0.0.0.0:6520"
BASE = {
    "/system/framework/services.jar": "6918c3c4e6233da19da9a4f7be88cbd5f7548f0300ff99b511dc5e62cba577df",
    "/system_ext/lib64/libvirtualcamera_relay_jni.so": "f9437642374a4c648973e8d2fe1b3b21e4d085fd97aaf388efd6d540f840214d",
    "/system/etc/vintf/compatibility_matrix.device.xml": "1eba00d7d4c6dc60e43716a4af14205c404947061af430be9803d98e2f5375ab",
}


def artifact_identity(badging):
    match = re.search(r"^package: name='([^']+)' versionCode='(\d+)'", badging, re.M)
    if match is None:
        raise ValueError("aapt did not identify the APEX container")
    return match.group(1), int(match.group(2))


def active_apex(xml):
    matches = [entry.attrib for entry in ET.fromstring(xml).findall("apex-info")
               if entry.get("moduleName") == PACKAGE and entry.get("isActive") == "true"]
    if len(matches) != 1:
        raise ValueError("Expected exactly one active virtual-camera APEX")
    return matches[0]


def sessions(text):
    compact = re.sub(r"\s+", "", text)  # Android wraps even field names at terminal width.
    parsed = []
    for match in re.finditer(r"sessionId=(\d+);(.*?)(?=sessionId=\d+;|$)", compact):
        fields = dict(re.findall(r"(\w+)=([^;]*);", match.group(2)))
        if not all(key in fields for key in ("isStaged", "isReady", "isApplied", "isFailed")):
            raise ValueError("Incomplete staged-session status")
        parsed.append({"id": int(match.group(1)), "package": fields.get("appPackageName", ""),
                       **{key: fields[key] == "true" for key in ("isStaged", "isReady", "isApplied", "isFailed")}})
    if compact and not parsed:
        raise ValueError("Unrecognized staged-session output")
    return parsed


def pending(items):
    return [item for item in items if item["isStaged"] and not item["isApplied"] and not item["isFailed"]]


class Installer:
    def __init__(self, args):
        self.args = args
        self.out = Path(args.output)
        self.out.mkdir(parents=True, exist_ok=False)
        self.result = {"schema": 1, "package": PACKAGE, "status": "NOT_STARTED",
                       "camera_validation": "NOT_RUN", "release_ready": False}
        self.log = (self.out / "commands.jsonl").open("w")

    def command(self, argv, timeout=30):
        completed = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        self.log.write(json.dumps({"argv": argv, "rc": completed.returncode,
                                   "stdout": completed.stdout, "stderr": completed.stderr}) + "\n")
        self.log.flush()
        status_query = argv[-5:] == ["shell", "pm", "list", "staged-sessions", "--only-parent"]
        # A13 runListStagedSessions returns 1 even on success. Do not extend
        # this exception to installs or any other command; parse status strictly.
        permitted = (0, 1) if status_query else (0,)
        if completed.returncode not in permitted or "Error:" in completed.stdout or (status_query and
                ("Failure [" in completed.stdout or (completed.returncode == 1 and completed.stderr.strip()))):
            raise RuntimeError(f"Command failed: {argv}: {completed.stdout} {completed.stderr}")
        return completed.stdout.strip()

    def adb(self, *args, timeout=30):
        return self.command([self.args.adb, "-s", self.args.serial, *args], timeout)

    def shell(self, *args):
        return self.adb("shell", *args)

    def run(self):
        if not self.args.allow_reference_update or self.args.serial != REFERENCE:
            raise ValueError("Explicit --allow-reference-update and exact reference VM serial required")
        artifact = Path(self.args.apex).resolve(strict=True)
        if artifact.suffix != ".apex":
            raise ValueError("Expected an uncompressed .apex artifact")
        digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if not re.fullmatch(r"[0-9a-f]{64}", self.args.expected_sha256) or digest != self.args.expected_sha256:
            raise ValueError("Artifact SHA-256 does not match the approved receipt")
        name, version = artifact_identity(self.command([self.args.aapt, "dump", "badging", str(artifact)]))
        if name != PACKAGE or version != self.args.version:
            raise ValueError("Wrong APEX package or expected version")
        self.result.update({"artifact": str(artifact), "sha256": digest, "version": version})
        if self.shell("getprop", "ro.product.name") != "aosp_cf_x86_64_phone" or self.shell("getprop", "ro.build.version.sdk") != "33":
            raise ValueError("Not the approved A13 Cuttlefish product")
        if self.shell("getprop", "ro.apex.updatable") != "true":
            raise ValueError("Device APEX installation is disabled")
        if self.shell("getenforce") != "Enforcing":
            raise ValueError("Reference must remain SELinux Enforcing")
        for path, required in BASE.items():
            if self.shell("sha256sum", path).split()[0] != required:
                raise ValueError("Base is not the validated V4 cohort: " + path)
        before = active_apex(self.shell("cat", "/apex/apex-info-list.xml"))
        if before.get("preinstalledModulePath") != "/vendor/apex/" + PACKAGE + ".apex":
            raise ValueError("Unexpected factory integration")
        if version <= int(before["versionCode"]):
            raise ValueError("Update must be newer; helper never permits downgrade/equal-version install")
        old_sessions = sessions(self.shell("pm", "list", "staged-sessions", "--only-parent"))
        if pending(old_sessions):
            raise ValueError("Another staged session is pending; will not reboot/activate it")
        self.result["before"] = before
        self.adb("install", "--staged", "-i", "com.android.shell", "--staged-ready-timeout", "15000", str(artifact), timeout=45)
        after = sessions(self.shell("pm", "list", "staged-sessions", "--only-parent"))
        known = {item["id"] for item in old_sessions}
        new = [item for item in after if item["id"] not in known and item["package"] == PACKAGE]
        if len(new) != 1 or not new[0]["isReady"] or new[0]["isFailed"]:
            raise RuntimeError("New staged session was not uniquely identified as READY")
        item = new[0]
        if any(other["id"] != item["id"] for other in pending(after)):
            raise RuntimeError("Another staged session appeared; refusing reboot")
        self.result.update({"status": "READY_REBOOT_REQUIRED", "session_id": item["id"]})
        self.save()
        if not self.args.reboot:
            return
        self.adb("reboot")
        deadline = time.monotonic() + 180
        while True:
            if time.monotonic() >= deadline:
                raise RuntimeError("Timed out awaiting reference boot; inspect staged status/recovery")
            time.sleep(1)
            try:
                self.adb("wait-for-device", timeout=20)
                if self.shell("getprop", "sys.boot_completed") == "1":
                    break
            except (RuntimeError, subprocess.TimeoutExpired):
                continue
        activated = active_apex(self.shell("cat", "/apex/apex-info-list.xml"))
        applied = [entry for entry in sessions(self.shell("pm", "list", "staged-sessions", "--only-parent"))
                   if entry["id"] == item["id"]]
        if len(applied) != 1 or not applied[0]["isApplied"] or applied[0]["isFailed"]:
            raise RuntimeError("Session did not become APPLIED; inspect rollback/activation failure")
        if int(activated["versionCode"]) != version or activated.get("isFactory") != "false" or not activated["modulePath"].startswith("/data/apex/active/"):
            raise RuntimeError("Expected updated data APEX is not active")
        self.result.update({"status": "APPLIED", "after": activated,
                            "selinux": self.shell("getenforce")})

    def save(self):
        (self.out / "result.json").write_text(json.dumps(self.result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--aapt", default="aapt")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--apex", required=True)
    parser.add_argument("--version", type=int, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--output", required=True, help="new private evidence directory")
    parser.add_argument("--allow-reference-update", action="store_true")
    parser.add_argument("--reboot", action="store_true")
    args = parser.parse_args()
    installer = Installer(args)
    code = 0
    try:
        installer.run()
        print(installer.result["status"])
    except Exception as error:
        installer.result.update({"status": "FAILED", "error": str(error)})
        print("FAILED:", error, file=sys.stderr)
        code = 1
    finally:
        installer.save()
        installer.log.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
