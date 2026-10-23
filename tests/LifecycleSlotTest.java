package com.android.server.camera.virtual;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class LifecycleSlotTest {
    private static void check(boolean value) { if (!value) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        LifecycleSlot<Object> slot = new LifecycleSlot<>();
        Object a = new Object(), b = new Object();
        check(slot.reserve(a) && slot.current() == a && slot.owns(a));
        check(!slot.reserve(b) && !slot.beginClose(b));
        check(slot.beginClose(a) && slot.current() == null && !slot.owns(a));
        check(!slot.reserve(b)); // original bug: early registry removal must NOT make the slot reusable
        check(!slot.finishClose(b) && slot.finishClose(a) && slot.reserve(b));
        check(!slot.finishClose(a) && !slot.beginClose(a) && slot.current() == b);
        int cases = 3;
        LifecycleSlot<Object> epochs = new LifecycleSlot<>();
        check(epochs.reserve(a));
        long configured = epochs.nextConfigurationEvent(a, 1, 20, false);
        check(configured > 0 && epochs.nextConfigurationEvent(a, 1, 10, true) == 0);
        check(epochs.isCurrentEvent(a, configured));
        check(epochs.nextConfigurationEvent(a, 1, 20, false) == 0);
        long closed = epochs.nextConfigurationEvent(a, 1, 30, true);
        check(closed > configured && epochs.nextConfigurationEvent(a, 1, 30, false) == 0);
        check(epochs.nextConfigurationEvent(a, 1, 20, false) == 0);
        check(epochs.nextConfigurationEvent(a, 2, 1, false) > closed);
        check(epochs.nextConfigurationEvent(a, 1, 100, true) == 0);
        ++cases;

        LifecycleSlot<Object> shared = new LifecycleSlot<>();
        check(shared.reserve(a));
        CountDownLatch cleaning = new CountDownLatch(1), release = new CountDownLatch(1);
        Thread cleanup = new Thread(() -> {
                check(shared.beginClose(a)); cleaning.countDown();
                try { check(release.await(2, TimeUnit.SECONDS)); }
                catch (InterruptedException e) { throw new AssertionError(e); }
                check(shared.finishClose(a));
        });
        cleanup.start(); check(cleaning.await(1, TimeUnit.SECONDS));
        check(!shared.reserve(b)); // forced pause between logical removal and external teardown
        release.countDown(); cleanup.join(1000); check(!cleanup.isAlive());
        check(shared.reserve(b) && shared.current() == b);
        ++cases;
        LifecycleSlot<Object> other = new LifecycleSlot<>();
        check(shared.beginClose(b) && other.reserve(a) && other.current() == a);
        check(shared.finishClose(b));
        ++cases;
        check(shared.reserve(a));
        long oldEvent = shared.nextEvent(a, 10);
        long newEvent = shared.nextEvent(a, 20);
        check(oldEvent != 0 && newEvent > oldEvent && !shared.isCurrentEvent(a, oldEvent));
        // Old callback passed its native check before reconnect, but reaches
        // Java after a newer-connection event: it must not cancel newer work.
        check(shared.nextEvent(a, 10) == 0 && shared.isCurrentEvent(a, newEvent));
        ++cases;
        check(shared.beginClose(a) && !shared.isCurrentEvent(a, newEvent) && shared.finishClose(a));
        check(shared.reserve(b));
        long bEvent = shared.nextEvent(b, 20);
        check(shared.nextEvent(a, 20) == 0 && shared.isCurrentEvent(b, bEvent));
        check(shared.beginClose(b) && shared.finishClose(b));
        ++cases;
        for (int i = 0; i < 10000; ++i) {
            check(shared.reserve(a) && shared.beginClose(a));
            check(!shared.reserve(b) && shared.finishClose(a));
            check(shared.reserve(b) && !shared.finishClose(a) && shared.current() == b);
            check(shared.beginClose(b) && shared.finishClose(b));
        }
        ++cases;
        System.out.println("PASS: " + cases + " slot ownership/teardown cases; 10000 stale-cleanup cycles");
    }
}
