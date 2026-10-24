package android.hardware.virtualcamera.hal;
import android.hardware.virtualcamera.hal.ProducerMode;
@VintfStability
parcelable ProducerDescriptor {
    long registration;
    int facing = 2;
    int orientation;
    int maxOutputStreams = 3;
    ProducerMode[] modes;
}
