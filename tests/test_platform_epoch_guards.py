"""Static production-wiring checks, not runtime Binder/GPU fault injection."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlatformEpochGuards(unittest.TestCase):
    def test_cleanup_finishes_before_slot_is_reusable(self):
        source = (ROOT / "service/VirtualCameraService.java").read_text()
        body = source[source.index("private void removeCamera"):source.index("/** Push availability")]
        self.assertLess(body.index("state.beginClose(camera)"), body.index("mCameras.remove(cameraId)"))
        # Descriptor-aware removal supersedes the old presence-only call.
        self.assertLess(body.index("mLifecycle.get(slot).execute"),
                        body.index("removeProducer(slot, camera.getCameraId())"))
        self.assertLess(body.index("removeProducer(slot, camera.getCameraId())"),
                        body.index("releaseSurface(slot)"))
        self.assertLess(body.index("releaseSurface(slot)"), body.index("state.finishClose(camera)"))
        self.assertLess(body.index("camera.close()"), body.index("state.finishClose(camera)"))

    def test_death_before_registry_publication_is_checked(self):
        source = (ROOT / "service/VirtualCameraService.java").read_text()
        self.assertLess(source.index("dead.set(true)"), source.index("mCameras.put(id, camera)"))
        self.assertLess(source.index("if (dead.get() ||"), source.index("mCameras.put(id, camera)"))

    def test_async_callbacks_preserve_connection_and_owner(self):
        source = (ROOT / "service/VirtualCameraService.java").read_text()
        self.assertEqual(source.count("final long connection = VirtualCameraNative.callbackEpoch()"), 3)
        self.assertIn("createSurfaceForEpoch(slot, width, height, connection, configuration)", source)
        self.assertIn("releaseSurfaceForEpoch(slot, connection, configuration)", source)
        self.assertIn("isCurrentEvent(slot, owner, event)", source)

    def test_connection_cookie_is_compared_before_global_clear(self):
        source = (ROOT / "platform-jni/VirtualCameraRelayJni.cpp").read_text()
        body = source[source.index("void onHalDiedCb"):source.index("AIBinder_DeathRecipient* deathRecipient")]
        self.assertLess(body.index("dead != gConnectionEpoch"), body.index("gHal.reset()"))
        self.assertIn("releaseQueue(slot, dead)", body)

    def test_listener_keeps_its_queue_and_gate_across_rpc(self):
        source = (ROOT / "platform-jni/VirtualCameraRelayJni.cpp").read_text()
        start = source.index("class FrameListener :")
        body = source[start:source.index("sp<IGraphicBufferProducer> createRelayQueue", start)]
        self.assertIn("const std::weak_ptr<RelayQueue> mQueue", body)
        self.assertLess(body.index("gSlots[mSlot] != queue"), body.index("acquireBuffer"))
        self.assertLess(body.index("gate(gSlotGates[mSlot])"), body.index("h->queueFrameForCamera"))
        self.assertIn("releaseHeld(*queue", body)
        self.assertIn("releaseBuffer(item, item.mFence)", body)
        self.assertIn("dependencyPreserved = acquire.get() >= 0", body)

    def test_abandon_before_dropping_held_buffer(self):
        source = (ROOT / "platform-jni/VirtualCameraRelayJni.cpp").read_text()
        body = source[source.index("void abandonQueueLocked"):source.index("void releaseQueue")]
        self.assertLess(body.index("consumer->abandon()"), body.index("held.reset()"))
        self.assertNotIn("releaseHeld", body)

    def test_scoped_native_methods_have_matching_signatures(self):
        source = (ROOT / "platform-jni/VirtualCameraRelayJni.cpp").read_text()
        self.assertIn('"nativeCallbackEpoch", "()J"', source)
        self.assertIn('"nativeIsCurrentConnection", "(J)Z"', source)
        self.assertIn('"nativeCreateSurfaceForEpoch", "(IIIJJ)Landroid/view/Surface;"', source)
        self.assertIn('"nativeReleaseSurfaceForEpoch", "(IJJ)V"', source)

    def test_registration_table_matches_java_native_declarations(self):
        java = (ROOT / "service/VirtualCameraNative.java").read_text()
        cpp = (ROOT / "platform-jni/VirtualCameraRelayJni.cpp").read_text()
        declarations = set(re.findall(r"private static native \w+ (native\w+)\(", java))
        registrations = set(re.findall(r'\{"(native\w+)",', cpp))
        self.assertEqual(declarations, registrations)
        self.assertNotIn("nativeCreateSurface", registrations)  # obsolete entry was stripped by R8
        self.assertNotIn("nativeSetCameraPresent", registrations)  # superseded by descriptor registration


if __name__ == "__main__":
    unittest.main()
