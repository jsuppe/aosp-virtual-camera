#!/usr/bin/env python3
"""Opt-in, pinned A13 Cuttlefish acceptance slices; resets named test apps.

Prerequisite: matched platform/HAL and both producer APKs plus VCamE2E installed.
Does not install artifacts, remount, change policy or target physical hardware.
HAL-restart scenario requires a second explicit flag and restarts only the named VM HAL.
Results distinguish this bounded slice from blocked release gates.
"""
import argparse
import json
import secrets
import subprocess
import sys
import time
from pathlib import Path

CONSUMER = "com.example.vcame2e"
PRODUCERS = ("com.example.vcamproducer", "com.example.vcamproducer2")
LIFECYCLE_CASES = ("E2E01_initial_absence", "E2E02_initial_no_frame", "E2E03_pixel_identity_and_freshness",
                   "E2E04_stall_without_false_success", "E2E05_resume_without_reopen",
                   "E2E06_two_distinct_pixel_streams", "E2E07_stall_isolation",
                   "E2E08_death_presence_and_isolation", "E2E09_new_registration_rejects_old_run",
                   "E2E10_reopen_configuration_freshness", "E2E11_negative_control_wrong_producer")
REOPEN_CASES = ("E2ERP01_baseline_freshness", "E2ERP02_paused_quiescence",
                "E2ERP03_reopen_while_paused", "E2ERP04_resume_fresh_configuration")
RESTART_CASES = ("E2EHR01_two_camera_baseline", "E2EHR02_real_hal_loss_notifications",
                 "E2EHR03_new_connection_pixel_recovery")


class Harness:
    def __init__(self, args):
        self.args = args
        self.out = Path(args.output)
        self.out.mkdir(parents=True, exist_ok=False)
        self.command_log = (self.out / "commands.log").open("w")
        self.run = secrets.randbelow(0x3fffffff) + 1
        self.command = self.run
        self.results = []
        self.last = None
        self.current_phase = None

    def adb(self, *args, check=True):
        cmd = [self.args.adb, "-s", self.args.serial, *args]
        result = subprocess.run(cmd, text=True, capture_output=True, timeout=25)
        self.command_log.write(json.dumps({"argv": cmd, "rc": result.returncode,
                                          "stdout": result.stdout, "stderr": result.stderr}) + "\n")
        self.command_log.flush()
        if check and (result.returncode or "Error:" in result.stdout):
            raise AssertionError(f"ADB operation failed: {args}: {result.stdout} {result.stderr}")
        return result.stdout.strip()

    def shell(self, *args, **kwargs):
        return self.adb("shell", *args, **kwargs)

    def preflight(self):
        if not self.args.allow_reference_mutations or self.args.serial != "0.0.0.0:6520":
            raise AssertionError("Explicit --allow-reference-mutations and exact reference VM serial required")
        if self.args.scenario == "hal-restart" and not self.args.allow_hal_restart:
            raise AssertionError("HAL-restart scenario also requires --allow-hal-restart")
        product = self.shell("getprop", "ro.product.name")
        sdk = self.shell("getprop", "ro.build.version.sdk")
        if product != "aosp_cf_x86_64_phone" or sdk != "33":
            raise AssertionError(f"Not the approved A13 reference VM: {product}/{sdk}")
        identity = {"serial": self.args.serial, "product": product, "sdk": sdk,
                    "fingerprint": self.shell("getprop", "ro.build.fingerprint"),
                    "selinux": self.shell("getenforce"),
                    "hidden_api_policy": self.shell("settings", "get", "global", "hidden_api_policy"),
                    "platform_hashes": self.shell("sha256sum", "/system/framework/services.jar",
                                                   "/system_ext/lib64/libvirtualcamera_relay_jni.so"),
                    "mounted_hal_hash": self.shell("su", "0", "sha256sum", "/apex/com.android.hardware.camera.provider.virtual/lib64/android.hardware.camera.provider-V1-virtual-impl.so", check=False),
                    "run": self.run}
        for package in (*PRODUCERS, CONSUMER):
            identity[package] = self.shell("dumpsys", "package", package)
            paths = self.shell("pm", "path", package)
            if not paths:
                raise AssertionError(f"Missing prerequisite APK {package}")
            identity[package + "_apk_hashes"] = [self.shell("sha256sum", line.removeprefix("package:"))
                                                for line in paths.splitlines()]
        (self.out / "identity.json").write_text(json.dumps(identity, indent=2))

    def stop_apps(self):
        for package in (*PRODUCERS, CONSUMER):
            self.shell("am", "force-stop", package)

    def consumer(self, **extras):
        self.command += 1
        args = ["am", "start", "-W", "-n", CONSUMER + "/.MainActivity",
                "--ei", "command", str(self.command)]
        for key, value in extras.items():
            args.extend(["--ez" if isinstance(value, bool) else "--ei", key, str(value).lower()])
        self.shell(*args)
        return self.wait(lambda s: s["command"] == self.command, "consumer control acknowledgment")

    def producer(self, number, paused=False, run=None, identity=None):
        package = PRODUCERS[number - 1]
        self.shell("am", "start-foreground-service", "-n",
                   package + "/com.example.vcamproducer.VCamProducerService",
                   "--ez", "e2e", "true", "--ei", "producer", str(identity or number),
                   "--ei", "run", str(run or self.run), "--ez", "paused", str(paused).lower())

    def state(self, allow_failure=False):
        raw = self.shell("run-as", CONSUMER, "cat", "files/status.json", check=False)
        try:
            state = json.loads(raw)
        except json.JSONDecodeError:
            return None
        # Ignore snapshots from a prior Activity/run until the command barrier is published.
        if state.get("command") != self.command:
            return None
        self.last = state
        if state["failure"] and not allow_failure:
            raise AssertionError("pixel/consumer oracle: " + state["failure"])
        return state

    def wait(self, predicate, description, timeout=15, allow_failure=False):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = self.state(allow_failure)
            if state is not None and predicate(state):
                return state
            time.sleep(.15)
        raise AssertionError(f"Timeout: {description}; last={self.last}")

    @staticmethod
    def cam(state, number):
        return state["cameras"][str(99 + number)]

    def advance(self, before, numbers, count=20):
        return self.wait(lambda s: all(self.cam(s, n)["images"] >= self.cam(before, n)["images"] + count
                                      for n in numbers), f"{count} new verified frames from {numbers}")

    def unchanged_lifecycle(self, before, after, number):
        for key in ("opens", "disconnects", "device_errors"):
            if self.cam(before, number)[key] != self.cam(after, number)[key]:
                raise AssertionError(f"Camera {number} unexpectedly changed {key}")

    def quiet(self, number):
        deadline = time.monotonic() + 8
        last = self.state()
        stable_at = time.monotonic()
        while time.monotonic() < deadline:
            time.sleep(.2)
            state = self.state()
            if state is None:
                continue
            if last is None or any(self.cam(state, number)[key] != self.cam(last, number)[key]
                                   for key in ("images", "completed")):
                stable_at = time.monotonic()
            elif time.monotonic() - stable_at >= .8:
                return state
            last = state
        raise AssertionError("No quiescent boundary after producer pause")

    def no_output(self, number, before, duration=2):
        # Allow in-flight work to drain BEFORE this window, not during it.
        deadline = time.monotonic() + duration
        state = before
        while time.monotonic() < deadline:
            time.sleep(.2)
            state = self.state()
            if state is None:
                continue
            for key in ("images", "completed", "opens", "disconnects", "device_errors"):
                if self.cam(state, number)[key] != self.cam(before, number)[key]:
                    raise AssertionError(f"Paused camera {number} changed {key}")
            if str(99 + number) not in state["inventory"] or not self.cam(state, number)["active"]:
                raise AssertionError("Temporary silence removed/closed a live producer camera")
        if self.cam(state, number)["capture_failures"] <= self.cam(before, number)["capture_failures"]:
            raise AssertionError("Vacuous no-output window: no failed capture attempts observed")
        return state

    def passed(self, name, state):
        self.results.append({"name": name, "status": "PASS", "snapshot": state})
        self.current_phase = None
        print("PASS:", name, flush=True)

    def prepare(self):
        self.stop_apps()
        self.shell("pm", "grant", CONSUMER, "android.permission.CAMERA")
        self.shell("input", "keyevent", "KEYCODE_WAKEUP")
        self.shell("wm", "dismiss-keyguard")
        self.consumer(run100=self.run, run101=self.run)

    def execute_reopen_paused(self):
        self.current_phase = REOPEN_CASES[0]
        self.prepare()
        before = self.wait(lambda s: not {"100", "101"}.intersection(s["inventory"]), "initial absence")
        self.producer(1)
        state = self.advance(before, (1,))
        self.passed(REOPEN_CASES[0], state)
        self.current_phase = REOPEN_CASES[1]
        self.producer(1, paused=True)
        state = self.no_output(1, self.quiet(1))
        self.passed(REOPEN_CASES[1], state)
        self.current_phase = REOPEN_CASES[2]
        before = state
        self.consumer(reopen100=True)
        state = self.wait(lambda s: self.cam(s, 1)["opens"] > self.cam(before, 1)["opens"]
                          and self.cam(s, 1)["active"]
                          and self.cam(s, 1)["capture_failures"] >= self.cam(before, 1)["capture_failures"] + 3,
                          "reopened paused session must fail captures, never replay old configuration")
        if self.cam(state, 1)["images"] != self.cam(before, 1)["images"]:
            raise AssertionError("Paused reopen delivered an old producer frame")
        state = self.no_output(1, state)
        self.passed(REOPEN_CASES[2], state)
        self.current_phase = REOPEN_CASES[3]
        self.producer(1)
        state = self.advance(state, (1,))
        self.passed(REOPEN_CASES[3], state)

    def execute_hal_restart(self):
        self.current_phase = RESTART_CASES[0]
        self.prepare()
        before = self.wait(lambda s: not {"100", "101"}.intersection(s["inventory"]), "initial absence")
        self.producer(1); self.producer(2)
        state = self.advance(before, (1, 2))
        producer_pids = [self.shell("pidof", package) for package in PRODUCERS]
        if not all(producer_pids):
            raise AssertionError("Missing baseline producer processes")
        self.passed(RESTART_CASES[0], state)
        self.current_phase = RESTART_CASES[1]
        before = state
        self.shell("su", "0", "setprop", "ctl.restart", "vendor.camera-provider-virtual")
        state = self.wait(lambda s: all(
            self.cam(s, n)["device_errors"] + self.cam(s, n)["disconnects"] >
            self.cam(before, n)["device_errors"] + self.cam(before, n)["disconnects"]
            for n in (1, 2)), "both open clients receive real HAL loss notification")
        self.passed(RESTART_CASES[1], state)
        self.current_phase = RESTART_CASES[2]
        state = self.wait(lambda s: all(self.cam(s, n)["opens"] > self.cam(before, n)["opens"]
                                       and self.cam(s, n)["active"] for n in (1, 2)), "both consumers reopen")
        state = self.advance(state, (1, 2))
        if producer_pids != [self.shell("pidof", package) for package in PRODUCERS]:
            raise AssertionError("Recovery restarted a producer instead of preserving its process")
        self.passed(RESTART_CASES[2], state)

    def execute(self):
        self.current_phase = "E2E01_initial_absence"
        self.prepare()
        state = self.wait(lambda s: not {"100", "101"}.intersection(s["inventory"]), "initial absence")
        if any(self.cam(state, n)["images"] for n in (1, 2)):
            raise AssertionError("Unexpected initial virtual frames")
        self.passed("E2E01_initial_absence", state)

        self.current_phase = "E2E02_initial_no_frame"
        self.producer(1, paused=True)
        state = self.wait(lambda s: self.cam(s, 1)["active"] and self.cam(s, 1)["capture_failures"] >= 3,
                          "initial no-frame capture attempts")
        if self.cam(state, 1)["images"] or self.cam(state, 1)["completed"]:
            raise AssertionError("Initial no-frame produced successful output")
        self.passed("E2E02_initial_no_frame", self.no_output(1, state))

        self.current_phase = "E2E03_pixel_identity_and_freshness"
        self.producer(1)
        before = state
        state = self.advance(state, (1,))
        self.unchanged_lifecycle(before, state, 1)
        self.passed("E2E03_pixel_identity_and_freshness", state)

        self.current_phase = "E2E04_stall_without_false_success"
        self.producer(1, paused=True)
        quiet = self.quiet(1)
        state = self.no_output(1, quiet)
        self.passed("E2E04_stall_without_false_success", state)
        self.current_phase = "E2E05_resume_without_reopen"
        self.producer(1)
        before = state
        state = self.advance(state, (1,))
        self.unchanged_lifecycle(before, state, 1)
        self.passed("E2E05_resume_without_reopen", state)

        self.current_phase = "E2E06_two_distinct_pixel_streams"
        self.producer(2)
        before = state
        state = self.advance(state, (1, 2))
        self.unchanged_lifecycle(before, state, 1)
        self.passed("E2E06_two_distinct_pixel_streams", state)
        self.current_phase = "E2E07_stall_isolation"
        self.producer(1, paused=True)
        quiet = self.quiet(1)
        state = self.no_output(1, quiet)
        state = self.advance(state, (2,))
        if self.cam(state, 1)["images"] != self.cam(quiet, 1)["images"]:
            raise AssertionError("Stalled camera delivered frames during isolation check")
        self.unchanged_lifecycle(quiet, state, 2)
        self.passed("E2E07_stall_isolation", state)

        self.current_phase = "E2E08_death_presence_and_isolation"
        before = state
        self.shell("am", "force-stop", PRODUCERS[0])
        state = self.wait(lambda s: "100" not in s["inventory"] and
                          self.cam(s, 1)["disconnects"] + self.cam(s, 1)["device_errors"] >
                          self.cam(before, 1)["disconnects"] + self.cam(before, 1)["device_errors"],
                          "producer death removes camera and notifies open client")
        state = self.advance(state, (2,))
        self.unchanged_lifecycle(before, state, 2)
        self.passed("E2E08_death_presence_and_isolation", state)

        self.current_phase = "E2E09_new_registration_rejects_old_run"
        self.consumer(run100=self.run + 1)
        self.producer(1, run=self.run + 1)
        before = state
        state = self.advance(state, (1, 2))
        self.unchanged_lifecycle(before, state, 2)
        self.passed("E2E09_new_registration_rejects_old_run", state)

        self.current_phase = "E2E10_reopen_configuration_freshness"
        before = state
        self.consumer(reopen100=True)
        state = self.advance(before, (1, 2))
        if self.cam(state, 1)["generation"] <= self.cam(before, 1)["generation"]:
            raise AssertionError("Reopen did not advance producer configuration identity")
        self.unchanged_lifecycle(before, state, 2)
        self.passed("E2E10_reopen_configuration_freshness", state)

        # A passing suite must also demonstrate that the live pixel oracle rejects wrong input.
        self.current_phase = "E2E11_negative_control_wrong_producer"
        self.producer(1, run=self.run + 1, identity=9)
        state = self.wait(lambda s: "100 pixels: java.lang.IllegalStateException: wrong producer/run 9/"
                          in s["failure"], "negative-control wrong-producer rejection", allow_failure=True)
        self.passed("E2E11_negative_control_wrong_producer", state)

    def report(self, error=None):
        if error:
            self.results.append({"name": self.current_phase or "execution", "status": "FAIL", "reason": str(error), "snapshot": self.last})
        attempted = {test["name"] for test in self.results}
        cases = {"reopen-paused": REOPEN_CASES, "hal-restart": RESTART_CASES,
                 "lifecycle": LIFECYCLE_CASES}[self.args.scenario]
        for name in cases:
            if name not in attempted:
                self.results.append({"name": name, "status": "NOT_RUN"})
        blocked = [
            "broader delayed-session callback Binder/JVM fault injection beyond controlled native/host cases",
            "producer capability and supported stream-combination matrix",
            "authorization, UID ownership and two-camera capacity policy",
            "real-GPU synchronization/transport/mixed-output fault qualification",
            "actual product app preview/record/still compatibility",
            "QCS8250 ARM64 latency, sustained reliability and physical-camera coexistence",
            "clean production base, signing, APEX update and recovery",
        ]
        result = {"schema": 1, "scope": "A13 reference YUV lifecycle/pixel acceptance slice",
                  "scenario": self.args.scenario,
                  "release_ready": False, "run": self.run, "tests": self.results,
                  "release_gates": [{"status": "BLOCKED", "name": name} for name in blocked],
                  "limits": ["640x480 YUV luma marker, not full-image/color/recording qualification",
                             "no-output windows exclude a bounded in-flight drain interval",
                             "no consumer delivery is not proof of zero GPU writes",
                             "automatic vertical-marker orientation detection is diagnostic, not orientation acceptance",
                             "local producer generation is not an authenticated HAL session token"]}
        (self.out / "results.json").write_text(json.dumps(result, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", required=True, help="new evidence directory; never overwrite a previous run")
    parser.add_argument("--allow-reference-mutations", action="store_true")
    parser.add_argument("--scenario", choices=("lifecycle", "reopen-paused", "hal-restart"), default="lifecycle")
    parser.add_argument("--allow-hal-restart", action="store_true")
    args = parser.parse_args()
    harness = Harness(args)
    error = None
    authorized = False
    try:
        harness.preflight()
        authorized = True
        if args.scenario == "reopen-paused":
            harness.execute_reopen_paused()
        elif args.scenario == "hal-restart":
            harness.execute_hal_restart()
        else:
            harness.execute()
    except Exception as exc:
        error = exc
        print("FAIL:", exc, file=sys.stderr, flush=True)
    finally:
        if authorized:
            for name, command in (
                ("camera-final.txt", ("dumpsys", "media.camera")),
                ("runtime.log", ("logcat", "-d", "-v", "threadtime", "-s", "VCamE2E", "VCamProducer", "VCamRelayJni", "VirtualCameraService")),
            ):
                try:
                    (harness.out / name).write_text(harness.shell(*command, check=False))
                except Exception as exc:
                    if error is None:
                        error = exc
            try:
                harness.stop_apps()
            except Exception as exc:
                if error is None:
                    error = exc
        harness.report(error)
        harness.command_log.close()
    return 1 if error else 0


if __name__ == "__main__":
    sys.exit(main())
