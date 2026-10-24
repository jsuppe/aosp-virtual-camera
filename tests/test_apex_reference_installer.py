"""Host guards and state parsing for the real reference staged-install helper."""
import argparse
import hashlib
import io
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("vcam_install", Path(__file__).resolve().parents[1] / "scripts/install-apex-reference.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
READY = "sessionId = 42; appPackageName = " + module.PACKAGE + "; isStaged = true; isReady = true; i\nsApplied = false; isFailed = false; errorMsg = ;"
APPLIED = READY.replace("isReady = true", "isReady = false").replace("i\nsApplied = false", "isApplied = true")


def xml(version, factory):
    path = "/vendor/apex/" + module.PACKAGE + ".apex" if factory else "/data/apex/active/" + module.PACKAGE + "@" + str(version) + ".apex"
    return '<apex-info-list><apex-info moduleName="' + module.PACKAGE + '" versionCode="' + str(version) + '" isActive="true" isFactory="' + str(factory).lower() + '" modulePath="' + path + '" preinstalledModulePath="/vendor/apex/' + module.PACKAGE + '.apex" /></apex-info-list>'


class ReferenceInstaller(unittest.TestCase):
    def fixture(self, reboot=False, other_pending=False):
        temporary = tempfile.TemporaryDirectory(prefix="vcam-install-test-")
        self.addCleanup(temporary.cleanup)
        apex = Path(temporary.name) / "candidate.apex"
        apex.write_bytes(b"host fixture, never installed")
        args = argparse.Namespace(adb="adb", aapt="aapt", serial=module.REFERENCE,
            apex=str(apex), version=5, expected_sha256=hashlib.sha256(apex.read_bytes()).hexdigest(),
            output=str(Path(temporary.name) / "evidence"), allow_reference_update=True, reboot=reboot)
        installer = module.Installer(args)
        self.addCleanup(installer.log.close)
        state = {"installed": False, "booted": False}
        commands = []

        def command(argv, timeout=30):
            commands.append(argv)
            if argv[0] == "aapt":
                return "package: name='" + module.PACKAGE + "' versionCode='5'"
            action = argv[3:]
            if action[:1] == ["install"]:
                state["installed"] = True
                return "Success. Reboot device to apply staged session"
            if action == ["reboot"]:
                state["booted"] = True
                return ""
            if action == ["wait-for-device"]:
                return ""
            if action[:2] == ["shell", "getprop"]:
                return {"ro.product.name": "aosp_cf_x86_64_phone", "ro.build.version.sdk": "33",
                        "ro.apex.updatable": "true", "sys.boot_completed": "1"}[action[2]]
            if action == ["shell", "getenforce"]:
                return "Enforcing"
            if action[:2] == ["shell", "sha256sum"]:
                return module.BASE[action[2]] + "  " + action[2]
            if action == ["shell", "cat", "/apex/apex-info-list.xml"]:
                return xml(5 if state["booted"] else 4, False)
            if action[:4] == ["shell", "pm", "list", "staged-sessions"]:
                if other_pending:
                    return READY.replace(module.PACKAGE, "other.module")
                return APPLIED if state["booted"] else READY if state["installed"] else ""
            raise AssertionError("Unexpected command " + repr(argv))
        installer.command = command
        return installer, commands

    def test_wrapped_session_fields(self):
        result = module.sessions(READY)[0]
        self.assertTrue(result["isReady"])
        self.assertFalse(result["isApplied"])
        self.assertEqual(result["id"], 42)

    def test_applied_failed_sessions_do_not_block_new_update(self):
        failed = READY.replace("isFailed = false", "isFailed = true")
        self.assertEqual(module.pending(module.sessions(APPLIED + failed)), [])

    def test_incomplete_session_is_not_assumed_ready(self):
        with self.assertRaises(ValueError):
            module.sessions("sessionId=1;isStaged=true;")

    def test_unknown_status_output_is_not_empty_session_list(self):
        with self.assertRaises(ValueError):
            module.sessions("Unknown command")

    def test_a13_read_status_exit_one_is_accepted_but_install_exit_one_is_not(self):
        installer = module.Installer.__new__(module.Installer)
        installer.log = io.StringIO()
        result = SimpleNamespace(returncode=1, stdout=READY, stderr="")
        with patch.object(module.subprocess, "run", return_value=result):
            self.assertEqual(installer.command(["adb", "-s", module.REFERENCE, "shell", "pm", "list", "staged-sessions", "--only-parent"]), READY)
            with self.assertRaises(RuntimeError):
                installer.command(["adb", "-s", module.REFERENCE, "install", "candidate.apex"])

    def test_transport_error_is_not_an_empty_successful_session_list(self):
        installer = module.Installer.__new__(module.Installer)
        installer.log = io.StringIO()
        result = SimpleNamespace(returncode=1, stdout="", stderr="adb: device offline")
        with patch.object(module.subprocess, "run", return_value=result):
            with self.assertRaises(RuntimeError):
                installer.command(["adb", "-s", module.REFERENCE, "shell", "pm", "list", "staged-sessions", "--only-parent"])

    def test_active_data_apex(self):
        result = module.active_apex(xml(5, False))
        self.assertEqual(result["versionCode"], "5")
        self.assertTrue(result["modulePath"].startswith("/data/apex/active/"))

    def test_wrong_or_absent_metadata_rejected(self):
        with self.assertRaises(ValueError):
            module.artifact_identity("not an APEX")
        self.assertEqual(module.artifact_identity("package: name='wrong.package' versionCode='9'"), ("wrong.package", 9))

    def test_stage_only_never_reboots(self):
        installer, commands = self.fixture()
        installer.run()
        self.assertEqual(installer.result["status"], "READY_REBOOT_REQUIRED")
        self.assertFalse(any(argv[3:] == ["reboot"] for argv in commands))

    def test_reboot_verifies_applied_data_version_without_root_or_remount(self):
        installer, commands = self.fixture(reboot=True)
        with patch.object(module.time, "sleep"):
            installer.run()
        self.assertEqual(installer.result["status"], "APPLIED")
        self.assertEqual(installer.result["camera_validation"], "NOT_RUN")
        self.assertFalse(any(word in ("root", "remount", "-d", "--force-non-staged") for argv in commands for word in argv))

    def test_physical_target_rejected_before_any_command(self):
        installer, commands = self.fixture()
        installer.args.serial = "physical"
        with self.assertRaises(ValueError):
            installer.run()
        self.assertEqual(commands, [])

    def test_wrong_hash_rejected_before_any_command(self):
        installer, commands = self.fixture()
        installer.args.expected_sha256 = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            installer.run()
        self.assertEqual(commands, [])

    def test_equal_version_never_installed(self):
        installer, commands = self.fixture()
        installer.args.version = 4
        original = installer.command
        installer.command = lambda argv, timeout=30: "package: name='" + module.PACKAGE + "' versionCode='4'" if argv[0] == "aapt" else original(argv, timeout)
        with self.assertRaisesRegex(ValueError, "newer"):
            installer.run()
        self.assertFalse(any(argv[3:4] == ["install"] for argv in commands))

    def test_other_pending_update_prevents_install_and_reboot(self):
        installer, commands = self.fixture(reboot=True, other_pending=True)
        with self.assertRaisesRegex(ValueError, "Another staged"):
            installer.run()
        self.assertFalse(any(argv[3:4] in (["install"], ["reboot"]) for argv in commands))


if __name__ == "__main__":
    unittest.main()
