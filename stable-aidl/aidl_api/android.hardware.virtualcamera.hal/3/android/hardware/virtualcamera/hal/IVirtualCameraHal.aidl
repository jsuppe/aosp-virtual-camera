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
///////////////////////////////////////////////////////////////////////////////
// THIS FILE IS IMMUTABLE. DO NOT EDIT IN ANY CASE.                          //
///////////////////////////////////////////////////////////////////////////////

// This file is a snapshot of an AIDL file. Do not edit it manually. There are
// two cases:
// 1). this is a frozen version file - do not edit this in any case.
// 2). this is a 'current' file. If you make a backwards compatible change to
//     the interface (from the latest frozen version), the build system will
//     prompt you to update this file with `m <name>-update-api`.
//
// You must not make a backward incompatible change to any AIDL file built
// with the aidl_interface module type with versions property set. The module
// type is used to build AIDL files in a way that they can be used across
// independently updatable components of the system. If a device is shipped
// with such a backward incompatible change, it has a high risk of breaking
// later when a module using the interface is updated, e.g., Mainline modules.

package android.hardware.virtualcamera.hal;
@VintfStability
interface IVirtualCameraHal {
  void setCallback(in android.hardware.virtualcamera.hal.IVirtualCameraHalCallback callback);
  void setProducerAvailable(boolean available);
  void queueFrame(in android.hardware.common.NativeHandle buffer, int width, int height, int stride, int format, long usage, long timestampNs);
  @nullable ParcelFileDescriptor queueFrameFenced(in android.hardware.common.NativeHandle buffer, int width, int height, int stride, int format, long usage, long timestampNs, in @nullable ParcelFileDescriptor acquireFence);
  int getMaxCameras();
  void setCameraPresent(int slot, boolean present);
  @nullable ParcelFileDescriptor queueFrameForCamera(int slot, in android.hardware.common.NativeHandle buffer, int width, int height, int stride, int format, long usage, long timestampNs, in @nullable ParcelFileDescriptor acquireFence);
}
