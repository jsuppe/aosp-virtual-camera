package com.android.server.camera.virtual;

import android.hardware.virtualcamera.ProducerMode;
import android.hardware.virtualcamera.VirtualCameraConfig;
import android.os.Parcel;
import java.util.ArrayList;
import java.util.HashSet;

/** Validated immutable-by-copy declaration; mirrors the vendor's resource bounds. */
final class ProducerCapabilities {
    static VirtualCameraConfig copy(VirtualCameraConfig config) {
        Parcel p = Parcel.obtain();
        try { config.writeToParcel(p, 0); p.setDataPosition(0); return VirtualCameraConfig.CREATOR.createFromParcel(p); }
        finally { p.recycle(); }
    }
    static VirtualCameraConfig normalize(VirtualCameraConfig supplied) {
        if (supplied == null) throw new IllegalArgumentException("missing descriptor");
        VirtualCameraConfig c = copy(supplied);
        if(c.supportedFormats != null && (c.supportedFormats.length != 1 || c.supportedFormats[0] != 1))
            throw new IllegalArgumentException("only RGBA8888 source transport is implemented");
        if (c.modes == null || c.modes.length == 0) {
            if (c.supportedFormats == null || c.supportedFormats.length != 1 || c.supportedFormats[0] != 1)
                throw new IllegalArgumentException("legacy source must declare RGBA8888");
            ProducerMode m = new ProducerMode(); m.id = 1; m.width = c.maxWidth; m.height = c.maxHeight;
            m.minFps = m.maxFps = c.maxFps;
            m.outputFormats = c.supportsJpeg ? new int[]{1,34,35,33} : new int[]{1,34,35};
            c.modes = new ProducerMode[]{m};
        }
        if (c.modes.length > 32 || c.facing < 0 || c.facing > 2 ||
            (c.orientation != 0 && c.orientation != 90 && c.orientation != 180 && c.orientation != 270) ||
            c.maxOutputStreams < 1 || c.maxOutputStreams > 3) throw new IllegalArgumentException("invalid descriptor bounds");
        HashSet<Integer> ids = new HashSet<>();
        for (ProducerMode m : c.modes) {
            if (m == null || m.id <= 0 || !ids.add(m.id) || m.width < 16 || m.height < 16 ||
                m.width > 8192 || m.height > 8192 || (m.width & 1) != 0 || (m.height & 1) != 0 ||
                (long)m.width * m.height > 3840L * 2160 || m.minFps < 1 || m.maxFps < m.minFps ||
                m.maxFps > 120 || m.outputFormats == null || m.outputFormats.length < 1 || m.outputFormats.length > 4)
                throw new IllegalArgumentException("invalid producer mode");
            HashSet<Integer> formats = new HashSet<>();
            for (int f : m.outputFormats) if ((f != 1 && f != 34 && f != 35 && f != 33) ||
                !formats.add(f) || (f == 33 && !c.supportsJpeg)) throw new IllegalArgumentException("unsupported conversion");
        }
        return c;
    }
    static int[] pack(VirtualCameraConfig c) {
        ArrayList<Integer> v = new ArrayList<>();
        v.add(c.facing); v.add(c.orientation); v.add(c.maxOutputStreams); v.add(c.modes.length);
        for (ProducerMode m : c.modes) {
            for (int n : new int[]{m.id,m.width,m.height,m.minFps,m.maxFps,m.allowDownscale?1:0,m.outputFormats.length}) v.add(n);
            for (int f : m.outputFormats) v.add(f);
        }
        int[] result = new int[v.size()]; for (int i=0;i<v.size();i++) result[i]=v.get(i); return result;
    }
}
