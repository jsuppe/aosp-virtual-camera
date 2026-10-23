package com.example.vcamtest;

import java.nio.ByteBuffer;

public final class FrameIdentityTest {
    private static ByteBuffer pixels(byte[] bits, int rowStride, int pixelStride, boolean flip, int offset) {
        int width = 320, height = 240;
        ByteBuffer buffer = ByteBuffer.allocate(offset + rowStride * height);
        buffer.position(offset);
        for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
            int row = (flip ? height - y - 1 : y) * 8 / height;
            boolean high = FrameIdentity.bit(bits, row * 16 + x * 16 / width);
            buffer.put(offset + y * rowStride + x * pixelStride, (byte) (high ? 235 : 16));
        }
        return buffer;
    }

    private static void require(boolean value) { if (!value) throw new AssertionError(); }

    private static void rejected(Runnable action) {
        try { action.run(); } catch (IllegalArgumentException expected) { return; }
        throw new AssertionError("invalid identity accepted");
    }

    public static void main(String[] args) {
        byte[] bits = FrameIdentity.encode(2, 0x12345678, 65535, 0xffffffffL);
        for (int pixelStride : new int[] {1, 2}) for (boolean flip : new boolean[] {false, true}) {
            int stride = 320 * pixelStride + 48;
            FrameIdentity id = FrameIdentity.decode(pixels(bits, stride, pixelStride, flip, 17),
                    320, 240, stride, pixelStride);
            require(id.producer == 2 && id.run == 0x12345678 && id.generation == 65535
                    && id.sequence == 0xffffffffL && id.flipped == flip);
        }
        for (int cell = 0; cell < 128; cell++) {
            byte[] broken = bits.clone();
            broken[cell / 8] ^= (byte) (1 << (7 - cell % 8));
            rejected(() -> FrameIdentity.decode(pixels(broken, 320, 1, false, 0), 320, 240, 320, 1));
        }
        rejected(() -> FrameIdentity.decode(ByteBuffer.allocate(320 * 240), 320, 240, 320, 1));
        rejected(() -> FrameIdentity.encode(0, 1, 1, 1));
        rejected(() -> FrameIdentity.encode(256, 1, 1, 1));
        rejected(() -> FrameIdentity.encode(1, 0, 1, 1));
        rejected(() -> FrameIdentity.encode(1, 1, 0, 1));
        rejected(() -> FrameIdentity.encode(1, 1, 65536, 1));
        rejected(() -> FrameIdentity.encode(1, 1, 1, 0));
        rejected(() -> FrameIdentity.encode(1, 1, 1, 0x100000000L));
        System.out.println("PASS: 4 stride/offset/orientation roundtrips, 128 single-bit corruption rejections, blank and 7 range rejections");
    }
}
