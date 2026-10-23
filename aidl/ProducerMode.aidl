package android.hardware.virtualcamera;
/** Explicit RGBA source mode; outputFormats describes supported conversion outputs. @hide */
parcelable ProducerMode {
    int id;
    int width;
    int height;
    int minFps;
    int maxFps;
    int[] outputFormats;
    boolean allowDownscale = false;
}
