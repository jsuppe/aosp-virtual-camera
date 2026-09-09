/*
 * Stable control + frame-transport interface between the platform
 * VirtualCameraService (system_server) and the vendor virtual-camera HAL.
 *
 * Direction of calls is system -> vendor (normal Treble direction); the HAL
 * reaches back only through the registered IVirtualCameraHalCallback.
 *
 * Frames cross the boundary zero-copy: the platform side owns the
 * BufferQueue (libgui is platform-only) and forwards each consumed graphics
 * buffer as a NativeHandle + description; the HAL imports it via
 * AHardwareBuffer_createFromHandle and keeps a reference until the next
 * frame replaces it.
 */
package android.hardware.virtualcamera.hal;

import android.hardware.virtualcamera.hal.IVirtualCameraHalCallback;
import android.hardware.common.NativeHandle;

@VintfStability
interface IVirtualCameraHal {
    /** Register the platform callback (stream lifecycle notifications). */
    void setCallback(in IVirtualCameraHalCallback callback);

    /**
     * Producer availability push: true when >=1 producer app is registered
     * with VirtualCameraService. The HAL adds/removes the virtual camera
     * device (cameraDeviceStatusChange) accordingly.
     */
    void setProducerAvailable(boolean available);

    /**
     * Deliver the newest producer frame. The handle refers to a gralloc
     * buffer; ownership of the passed fds stays with the caller (the HAL
     * clones via AHardwareBuffer_createFromHandle). RGBA_8888 assumed.
     */
    void queueFrame(in NativeHandle buffer, int width, int height,
                    int stride, int format, long usage, long timestampNs);

    /**
     * V2: fenced delivery. Same buffer semantics as queueFrame(), plus:
     *
     *   acquireFence  - the producer's GPU-completion fence for this buffer
     *                   (null when the buffer is already complete). The HAL
     *                   waits on it (GPU-side where possible) before sampling.
     *
     *   return value  - the HAL's read-completion fence for the PREVIOUSLY
     *                   queued buffer, which this call retires (the HAL keeps
     *                   only the newest frame). The platform must attach it as
     *                   the release fence when returning that buffer to its
     *                   BufferQueue, so the producer's next render into it
     *                   waits for the HAL's GPU read. null = safe to reuse now.
     */
    @nullable ParcelFileDescriptor queueFrameFenced(in NativeHandle buffer,
            int width, int height, int stride, int format, long usage,
            long timestampNs, in @nullable ParcelFileDescriptor acquireFence);
}
