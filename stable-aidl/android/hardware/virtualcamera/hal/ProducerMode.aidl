package android.hardware.virtualcamera.hal;
@VintfStability
parcelable ProducerMode {
    int id;
    int width;
    int height;
    int minFps;
    int maxFps;
    int[] outputFormats;
    boolean allowDownscale = false;
}
