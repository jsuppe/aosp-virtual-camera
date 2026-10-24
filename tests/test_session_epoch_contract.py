"""Structural V4 contract gate, NOT runtime race injection.

The archived batch-7 version failed against V3. Preserve old frozen interfaces;
runtime admission/relay and full-path tests must independently establish behavior.
"""
from pathlib import Path
import re
import unittest


class SessionEpochContract(unittest.TestCase):
    def test_hal_configuration_and_close_callbacks_identify_session(self):
        source = (Path(__file__).resolve().parents[1] / "stable-aidl/android/hardware/virtualcamera/hal/IVirtualCameraHalCallback.aidl").read_text()
        for name in ("onStreamsConfiguredForConfiguration", "onCameraClosedForConfiguration"):
            signature = re.search(r"\b" + name + r"\s*\(([^)]*)\)", source)
            self.assertIsNotNone(signature, name)
            self.assertRegex(signature.group(1), r"\blong configurationToken\b",
                             "BLOCKED: callback has no originating-session identity; slot/connection alone is insufficient")

    def test_frame_entry_and_frozen_v4_carry_token_without_rewriting_old_versions(self):
        root = Path(__file__).resolve().parents[1] / "stable-aidl"
        source = (root / "android/hardware/virtualcamera/hal/IVirtualCameraHal.aidl").read_text()
        self.assertRegex(source, r"queueFrameForConfiguration\(int slot, long configurationToken")
        api = root / "aidl_api/android.hardware.virtualcamera.hal"
        for version in ("1", "2", "3"):
            self.assertNotIn("queueFrameForConfiguration", (api / version / "android/hardware/virtualcamera/hal/IVirtualCameraHal.aidl").read_text())
        self.assertIn("queueFrameForConfiguration", (api / "4/android/hardware/virtualcamera/hal/IVirtualCameraHal.aidl").read_text())


if __name__ == "__main__":
    unittest.main()
