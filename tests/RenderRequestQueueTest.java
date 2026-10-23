package com.example.vcamproducer;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

public final class RenderRequestQueueTest {
    static final class Resource implements RenderRequestQueue.Resource {
        int releases;
        public void release() { if (++releases != 1) throw new AssertionError("double release"); }
    }
    private static void check(boolean b) { if (!b) throw new AssertionError(); }
    public static void main(String[] args) throws Exception {
        int cases = 0;
        RenderRequestQueue<Resource> queue = new RenderRequestQueue<>();
        Resource a = new Resource(), b = new Resource(), c = new Resource();
        queue.submit(a); queue.submit(b);
        check(a.releases == 1 && b.releases == 0);
        RenderRequestQueue.Request<Resource> active = queue.take();
        check(active.value == b && queue.isCurrent(active));
        ++cases;
        queue.submit(c);
        check(!queue.isCurrent(active) && b.releases == 0); // active resource belongs to worker
        active.value.release();
        RenderRequestQueue.Request<Resource> next = queue.take();
        check(next.value == c && queue.isCurrent(next) && next.generation > active.generation);
        ++cases;
        queue.cancel(); check(!queue.isCurrent(next) && c.releases == 0);
        next.value.release();
        Resource pending = new Resource(); queue.submit(pending); queue.cancel();
        check(pending.releases == 1);
        ++cases;
        CountDownLatch waiting = new CountDownLatch(1);
        AtomicBoolean exited = new AtomicBoolean();
        Thread consumer = new Thread(() -> {
            try { waiting.countDown(); check(queue.take() == null); exited.set(true); }
            catch (InterruptedException e) { throw new AssertionError(e); }
        });
        consumer.start(); check(waiting.await(1, TimeUnit.SECONDS));
        queue.shutdown(); consumer.join(1000);
        check(!consumer.isAlive() && exited.get());
        ++cases;
        Resource late = new Resource(); queue.submit(late);
        check(late.releases == 1 && queue.take() == null);
        ++cases;
        // Thousands of replacements must neither leak pending resources nor
        // make an already cancelled generation current again (ABA).
        RenderRequestQueue<Resource> storm = new RenderRequestQueue<>();
        Resource[] resources = new Resource[10000];
        for (int i = 0; i < resources.length; ++i) {
            resources[i] = new Resource(); storm.submit(resources[i]);
        }
        RenderRequestQueue.Request<Resource> last = storm.take();
        for (int i = 0; i < resources.length - 1; ++i) check(resources[i].releases == 1);
        check(last.value == resources[resources.length - 1] && storm.isCurrent(last));
        storm.shutdown(); check(!storm.isCurrent(last)); last.value.release();
        ++cases;
        System.out.println("PASS: " + cases + " renderer-mailbox ownership/cancellation cases; 10000 replacements");
    }
}
