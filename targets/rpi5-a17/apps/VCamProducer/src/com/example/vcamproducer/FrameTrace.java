package com.example.vcamproducer;

import java.io.*;

/** Bounded opt-in samples; export only after the measured stream stops. */
final class FrameTrace {
    private static final int LIMIT = 16384;
    private final long[] data;
    private final File file;
    private final String header;
    private int rows, dropped;
    private boolean written;
    FrameTrace(File directory, String name, String header, boolean enabled) {
        data = enabled ? new long[LIMIT * 8] : null;
        file = new File(directory, name + ".csv");
        this.header = header;
    }
    void add(long a, long b, long c, long d, long e, long f, long g, long h) {
        if (data == null) return;
        if (rows == LIMIT) { dropped++; return; }
        int i = rows++ * 8;
        data[i]=a; data[i+1]=b; data[i+2]=c; data[i+3]=d;
        data[i+4]=e; data[i+5]=f; data[i+6]=g; data[i+7]=h;
    }
    void close() throws IOException {
        if (data == null || written) return;
        if (!file.getParentFile().isDirectory() && !file.getParentFile().mkdirs())
            throw new IOException("Cannot create trace directory");
        if (!file.createNewFile()) throw new IOException("Trace already exists: " + file);
        try (BufferedWriter out = new BufferedWriter(new FileWriter(file))) {
            out.write("# schema=1 dropped=" + dropped + "\n" + header + "\n");
            for (int r=0; r<rows; r++) {
                for (int c=0; c<8; c++) {
                    if (c>0) out.write(',');
                    out.write(Long.toString(data[r*8+c]));
                }
                out.write('\n');
            }
        }
        written=true;
    }
}
