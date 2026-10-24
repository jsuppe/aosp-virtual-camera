"""Host negative controls for the actual reference runner's acceptance oracles."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("vcam_e2e", Path(__file__).resolve().parents[1] / "scripts/test-e2e-reference.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerOracles(unittest.TestCase):
    def setUp(self):
        self.harness = runner.Harness.__new__(runner.Harness)
        self.before = {"inventory": ["100"], "cameras": {"100": {
            "images": 20, "completed": 20, "opens": 1, "disconnects": 0,
            "device_errors": 0, "capture_failures": 1, "active": True}}}
        self.after = copy.deepcopy(self.before)
        self.after["cameras"]["100"]["capture_failures"] += 1
        self.harness.state = lambda: self.after

    def window(self):
        with patch.object(runner.time, "monotonic", side_effect=[0, .1, 3]), patch.object(runner.time, "sleep"):
            return self.harness.no_output(1, self.before)

    def test_active_error_requests_without_output_pass(self):
        self.window()

    def test_no_requests_is_not_a_pass(self):
        self.after["cameras"]["100"]["capture_failures"] = 1
        with self.assertRaisesRegex(AssertionError, "Vacuous"):
            self.window()

    def test_new_image_is_not_a_pass(self):
        self.after["cameras"]["100"]["images"] += 1
        with self.assertRaisesRegex(AssertionError, "images"):
            self.window()

    def test_false_success_is_not_a_pass(self):
        self.after["cameras"]["100"]["completed"] += 1
        with self.assertRaisesRegex(AssertionError, "completed"):
            self.window()

    def test_reopen_is_not_a_stall_pass(self):
        self.after["cameras"]["100"]["opens"] += 1
        with self.assertRaisesRegex(AssertionError, "opens"):
            self.window()

    def test_inventory_removal_is_not_a_stall_pass(self):
        self.after["inventory"] = []
        with self.assertRaisesRegex(AssertionError, "removed/closed"):
            self.window()

    def test_other_camera_lifecycle_is_checked(self):
        self.after["cameras"]["100"]["device_errors"] += 1
        with self.assertRaisesRegex(AssertionError, "device_errors"):
            self.harness.unchanged_lifecycle(self.before, self.after, 1)

    def test_physical_serial_rejected_before_adb(self):
        self.harness.args = type("Args", (), {"allow_reference_mutations": True, "serial": "physical-device"})()
        self.harness.shell = lambda *a, **kw: self.fail("must not contact physical device")
        with self.assertRaisesRegex(AssertionError, "exact reference VM"):
            self.harness.preflight()

    def test_failed_phase_is_not_reported_as_unrun(self):
        with tempfile.TemporaryDirectory(prefix="vcam-report-test-") as directory:
            self.harness.out = Path(directory)
            self.harness.args = type("Args", (), {"scenario": "reopen-paused"})()
            self.harness.run = 1
            self.harness.last = self.before
            self.harness.current_phase = runner.REOPEN_CASES[2]
            self.harness.results = [{"name": name, "status": "PASS"} for name in runner.REOPEN_CASES[:2]]
            self.harness.report(AssertionError("old configuration"))
            result = json.loads((Path(directory) / "results.json").read_text())
            self.assertFalse(result["release_ready"])
            self.assertEqual([test["status"] for test in result["tests"]], ["PASS", "PASS", "FAIL", "NOT_RUN"])
            self.assertTrue(all(gate["status"] == "BLOCKED" for gate in result["release_gates"]))

    def test_hal_restart_requires_second_explicit_flag_before_adb(self):
        self.harness.args = type("Args", (), {"allow_reference_mutations": True,
            "serial": "0.0.0.0:6520", "scenario": "hal-restart", "allow_hal_restart": False})()
        self.harness.shell = lambda *a, **kw: self.fail("must not contact device before restart authorization")
        with self.assertRaisesRegex(AssertionError, "also requires --allow-hal-restart"):
            self.harness.preflight()


if __name__ == "__main__":
    unittest.main()
