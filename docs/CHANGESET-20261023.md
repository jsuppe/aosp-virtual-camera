# Current camera change set — 2026-10-23

This branch publishes the previously local pending source changes from the Android13 platform camera reference together with the isolated Android17 Raspberry Pi5 port. Original source/worktree and validated Android13 artifacts were preserved.

## Shared pending source now included

- Stable HAL AIDL versions4,5,6 retained/frozen, including producer descriptors, modes and registration/configuration identity. Package/APEX version7 is separate from interface version6.
- Descriptor-derived size/format/FPS negotiation, disjoint FPS range handling and source readiness scoped to owner/connection/configuration.
- Broker slot teardown reservation, asynchronous epoch guards and JNI registration/R8 cleanup.
- Acquire/destination/read-fence validation, source leases and safe read-fence retirement/merge-failure behavior.
- Frame-identity pixel oracle, public Camera2 test app, reference staged-APEX installer and lifecycle/negotiation/host test helpers.
- QCS8250 Android13 bring-up notes.

## Pi-specific current implementation

See [target documentation](../targets/rpi5-a17/README.md) and [test app](../targets/rpi5-a17/apps/VCamProducer).

- Android17 graphics consumer factory/common-helper API adaptation.
- Separate `ruminate_virtual_camera` broker name avoids the Android17 stock service-label collision.
- APEX-only VINTF declaration on Android17, removing the Android13 duplicate fallback.
- Android17 vendor HAL SELinux service attributes and explicit registration restriction, without disabling validation.
- Pi Mesa/minigbm GPU-buffer usage correction.
- Explicit user-started typed foreground service and launcher.
- Vsync-paced single-owner renderer; GPU-owned vertex buffer prevents stale pixels after the Java client array becomes unreachable.
- Direct producer/real Camera2 split comparison with frame-stamped text/clock, progress strip and one-window freeze/resume.
- Default VGA640x480 mode and split viewer now request60fps. Larger default modes remain30fps; optional higher-resolution diagnostic profiles are not proof of sustained throughput.
- Reproducible pinned source manifest, portable integration/full-image build helper, source-label/VINTF checks and rootless disk packager.

## Checks for this publication

Five existing host scripts pass: fence safety, frame identity, lifecycle slot, producer capabilities and renderer mailbox. Python unittest discovery passes48 tests. One stale source guard referred to obsolete presence-only removal; it now checks descriptor-aware producer removal, surface release and slot-finalization order. Original failure is retained in local evidence.

Both Pi producer APKs compile on the matched Android17 ARM64 checkout. Only first producer is tested on physical hardware. Target runtime results are recorded separately, distinguishing negotiation/callbacks, recorded pixel progression and performance/latency evidence. No claim that host source guards are runtime Binder/GPU fault tests.

The new target build wrapper is syntax-checked and its integration output is compared with the actual compiled inputs; a new complete factory-image build/flash is NOT implied by the APK update. Existing full R2 archives contain an earlier producer APK.

## Deliberate exclusions

No Android disk images, APK/video binaries, Python cache, private workspace memory, credentials or new signing keys are added by this change set. Development AVB key files already present in repository history are unchanged. This remains a development/userdebug target; upstream Pi SELinux-permissive and test-signing status are not production security qualification.
