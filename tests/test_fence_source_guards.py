"""Static wiring checks; these are NOT Android GPU/fence fault-injection tests."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FenceSourceGuards(unittest.TestCase):
    def test_both_gpu_paths_use_checked_wait(self):
        source = (ROOT / "hal/core/GpuCompositor.cpp").read_text()
        self.assertEqual(source.count("if (!waitForSourceFence(srcAcquireFence))"), 2)
        self.assertIn("if (fd < 0) return false;", source)
        self.assertNotIn("if (!waited) sync_wait", source)

    def test_jpeg_wait_failure_returns_before_source_lock(self):
        source = (ROOT / "hal/core/JpegEncoder.cpp").read_text()
        guard = source.index("if (!acquireFenceReady(acquireFenceFd")
        failure = source.index("return 0;", guard)
        self.assertLess(failure, source.index("AHardwareBuffer_lock(src"))

    def test_cpu_fallback_uses_checked_wait(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        self.assertIn("if (::virtualcamera::acquireFenceReady(acquireFence,", source)
        self.assertNotIn("if (acquireFence >= 0) sync_wait", source)

    def test_retirement_timeout_rejects_before_current_frame_swap(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraStableHal.cpp").read_text()
        start = source.index("if (!mCv.wait_for")
        reject = source.index("return ndk::ScopedAStatus::fromServiceSpecificError(4);", start)
        swap = source.index("old = std::move(cur);", start)
        self.assertLess(reject, swap)
        cleanup = source[start:reject]
        self.assertIn("AHardwareBuffer_release(ahb);", cleanup)
        self.assertIn("closeFd(acquireFenceFd);", cleanup)

    def test_merge_failure_rejects_before_old_frame_is_replaced(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraStableHal.cpp").read_text()
        start = source.index("if (!consolidateFences(cur.readFences))")
        reject = source.index("fromServiceSpecificError(6)", start)
        self.assertLess(reject, source.index("old = std::move(cur);", start))

    def test_retiring_frame_is_not_discarded_before_late_readers(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraStableHal.cpp").read_text()
        start = source.index("bool VirtualCameraStableHal::reapRetiringLocked")
        end = source.index("void VirtualCameraStableHal::dropLatestLocked", start)
        body = source[start:end]
        self.assertLess(body.index("retiredFrameReady"), body.index("AHardwareBuffer_release"))
        self.assertIn("cur.ahb == nullptr || cur.retiring", source)
        self.assertIn("cur.readFences.size() + cur.inflight >= kMaxPendingReads", source)

    def test_all_hal_acquire_fence_duplications_are_checked(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraStableHal.cpp").read_text()
        self.assertEqual(source.count("duplicateAcquireFence("), 4)  # V2, V3, V4, acquireLatest
        self.assertEqual(source.count("fromServiceSpecificError(5)"), 3)

    def test_destination_wait_is_before_shutter_and_all_target_writes(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        body = source[source.index("CameraStatus VirtualCameraSession::processSingleRequest"):]
        wait = body.index("sync_wait(fd, 100)")
        self.assertLess(wait, body.index("shutter.set<NotifyMsg::Tag::shutter>"))
        self.assertLess(wait, body.index("importBuffer(inBuffer)"))
        self.assertLess(body.index("int fd = ::dup(fence.fds[0].get());"), wait)
        self.assertLess(body.index("if (importBuffer(buffer) == nullptr)"), wait)

    def test_failed_destination_returns_error_buffers_and_dependencies(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        start = source.index("auto failUntouched = [&]()")
        end = source.index("// Destination dependencies", start)
        failure = source[start:end]
        self.assertIn("ErrorCode::ERROR_REQUEST", failure)
        self.assertIn("buffer.status = BufferStatus::ERROR;", failure)
        self.assertIn("pendingFences[j].release()", failure)
        self.assertIn("result.partialResult = 0;", failure)
        self.assertNotIn("ShutterMsg", failure)
        self.assertNotIn("buildResultMetadata", failure)
        self.assertIn("cancelled() || (fd >= 0 && sync_wait(fd, 100) != 0)", source)

    def test_no_synthetic_or_legacy_replay_on_capture_path(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        body = source[source.index("CameraStatus VirtualCameraSession::processSingleRequest"):]
        self.assertNotIn("fillYuvBufferFromRenderer", body)
        self.assertNotIn("fillBufferFromV2(", body)
        self.assertNotIn("fillLatestInto", body)
        self.assertEqual(body.count("hal->acquireLatest("), 1)
        self.assertIn("&sequence, mLastSourceSequence", body)
        self.assertIn("mLastSourceSequence = sequence", body)
        self.assertLess(body.index("if (!src || cancelled())"), body.index("importBuffer(inBuffer)"))

    def test_multi_output_source_lease_records_all_fences(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        self.assertIn("hal->releaseFrame(slot, buffer, std::move(fences))", source)
        self.assertIn("source.fences.push_back(doneFence)", source)
        self.assertIn("if (acquire.get() >= 0) fences.push_back(acquire.release())", source)

    def test_frame_identity_survives_slot_reset(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraStableHal.cpp").read_text()
        self.assertIn("cur.sequence = ++mFramesReceived", source)
        self.assertIn("cur.sequence <= afterSequence", source)

    def test_malformed_destination_fences_rejected_before_processing(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        self.assertIn("!b.acquireFence.ints.empty() || b.acquireFence.fds.size() > 1", source)
        self.assertIn("b.acquireFence.fds[0].get() < 0", source)

    def test_import_never_closes_borrowed_parcel_fds(self):
        source = (ROOT / "hal/aidl-v1/VirtualCameraSession.cpp").read_text()
        start = source.index("buffer_handle_t VirtualCameraSession::importBuffer")
        end = source.index("void VirtualCameraSession::removeBuffersFromCache", start)
        body = source[start:end]
        self.assertNotIn("native_handle_close", body)
        self.assertIn("const int inputFdCount = nativeHandle->numFds;", body)
        self.assertIn("native_handle_delete(nativeHandle);", body)


if __name__ == "__main__":
    unittest.main()
