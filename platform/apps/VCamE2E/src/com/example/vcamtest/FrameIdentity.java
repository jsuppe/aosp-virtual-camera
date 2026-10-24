package com.example.vcamtest;

import java.nio.ByteBuffer;

/** Diagnostic pixel protocol, NOT the HAL session-token contract. No Android dependencies. */
public final class FrameIdentity {
    public static final int COLS = 16, ROWS = 8;
    public final int producer, run, generation;
    public final long sequence;
    public final boolean flipped;

    private FrameIdentity(byte[] b, boolean flip) {
        producer = b[3] & 255;
        run = (int) unsigned(b, 4, 4);
        generation = (int) unsigned(b, 8, 2);
        sequence = unsigned(b, 10, 4);
        flipped = flip;
    }

    public static byte[] encode(int producer, int run, int generation, long sequence) {
        if (producer < 1 || producer > 255 || run <= 0 || generation < 1 || generation > 65535
                || sequence < 1 || sequence > 0xffffffffL) {
            throw new IllegalArgumentException("identity out of range");
        }
        byte[] b = new byte[16];
        b[0] = 0x56; b[1] = (byte) 0xca; b[2] = 1; b[3] = (byte) producer;
        put(b, 4, 4, run); put(b, 8, 2, generation); put(b, 10, 4, sequence);
        put(b, 14, 2, crc(b));
        return b;
    }

    public static boolean bit(byte[] b, int cell) {
        return ((b[cell / 8] >>> (7 - cell % 8)) & 1) != 0;
    }

    public static FrameIdentity decode(ByteBuffer y, int width, int height,
            int rowStride, int pixelStride) {
        if (width < COLS * 4 || height < ROWS * 4 || rowStride < width * pixelStride
                || pixelStride < 1) throw new IllegalArgumentException("invalid luma layout");
        for (boolean flip : new boolean[] {false, true}) {
            byte[] b = new byte[16];
            boolean contrast = true;
            for (int cell = 0; cell < 128; cell++) {
                int col = cell % COLS, row = cell / COLS;
                int sum = 0;
                // Sample cell interiors, never the potentially filtered boundaries.
                for (int sample = 0; sample < 5; sample++) {
                    int sx = (2 * col + 1) * width / (2 * COLS);
                    int sy = (2 * row + 1) * height / (2 * ROWS);
                    if (sample == 1) sx -= width / (COLS * 8);
                    if (sample == 2) sx += width / (COLS * 8);
                    if (sample == 3) sy -= height / (ROWS * 8);
                    if (sample == 4) sy += height / (ROWS * 8);
                    if (flip) sy = height - 1 - sy;
                    sum += y.get(y.position() + sy * rowStride + sx * pixelStride) & 255;
                }
                int value = sum / 5;
                if (value > 80 && value < 176) contrast = false;
                if (value >= 128) b[cell / 8] |= (byte) (1 << (7 - cell % 8));
            }
            if (contrast && b[0] == 0x56 && b[1] == (byte) 0xca && b[2] == 1
                    && unsigned(b, 14, 2) == crc(b)) {
                FrameIdentity id = new FrameIdentity(b, flip);
                if (id.producer > 0 && id.run > 0 && id.generation > 0 && id.sequence > 0) return id;
            }
        }
        throw new IllegalArgumentException("marker contrast/header/CRC failure");
    }

    /** RGBA diagnostic: every RGB channel must independently carry the same valid marker. */
    public static FrameIdentity decodeRgba(ByteBuffer pixels, int width, int height,
            int rowStride, int pixelStride) {
        if (pixelStride < 4) throw new IllegalArgumentException("invalid RGBA pixel stride");
        FrameIdentity first = null;
        for (int channel = 0; channel < 3; channel++) {
            ByteBuffer view = pixels.duplicate();
            view.position(pixels.position() + channel);
            FrameIdentity id = decode(view, width, height, rowStride, pixelStride);
            if (first == null) first = id;
            else if (id.producer != first.producer || id.run != first.run ||
                    id.generation != first.generation || id.sequence != first.sequence ||
                    id.flipped != first.flipped)
                throw new IllegalArgumentException("RGBA channel identity mismatch");
        }
        for (int row = 0; row < ROWS; row++) for (int col = 0; col < COLS; col++) {
            int x = (2 * col + 1) * width / (2 * COLS);
            int y = (2 * row + 1) * height / (2 * ROWS);
            if ((pixels.get(pixels.position() + y * rowStride + x * pixelStride + 3) & 255) != 255)
                throw new IllegalArgumentException("RGBA marker alpha corruption");
        }
        return first;
    }

    private static long unsigned(byte[] b, int start, int length) {
        long value = 0;
        for (int i = start; i < start + length; i++) value = (value << 8) | (b[i] & 255);
        return value;
    }

    private static void put(byte[] b, int start, int length, long value) {
        for (int i = start + length - 1; i >= start; i--, value >>>= 8) b[i] = (byte) value;
    }

    private static int crc(byte[] b) {
        int crc = 0xffff;
        for (int i = 0; i < 14; i++) {
            crc ^= (b[i] & 255) << 8;
            for (int bit = 0; bit < 8; bit++) crc = ((crc << 1) ^ ((crc & 0x8000) == 0 ? 0 : 0x1021)) & 65535;
        }
        return crc;
    }
}
