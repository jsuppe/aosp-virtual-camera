package com.example.vcame2e;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.graphics.ImageFormat;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.TotalCaptureResult;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.AtomicFile;
import android.util.Log;
import android.util.Size;
import android.widget.TextView;

import com.example.vcamtest.FrameIdentity;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

/** Public-SDK Camera2 acceptance probe. Only the shell runner controls test expectations. */
public final class MainActivity extends Activity {
    private static final String TAG = "VCamE2E";
    private HandlerThread thread;
    private Handler handler;
    private CameraManager manager;
    private AtomicFile output;
    private final Slot[] slots = {new Slot("100", 1), new Slot("101", 2)};
    private Set<String> inventory = new HashSet<>();
    private String failure = "";
    private int command;
    private boolean stopping;
    private boolean probeOnly;

    private static final class Slot {
        final String id;
        final int producer;
        int width=640,height=480,fps=30;
        int expectedRun, opens, disconnects, deviceErrors, generation, lastDeviceError, minGeneration = 1;
        long images, completed, captureFailures, bufferLost, sequence, timestamp, openSerial, frameDuration;
        boolean flipped;
        Attempt attempt;
        Slot(String id, int producer) { this.id = id; this.producer = producer; }
    }

    private static final class Attempt {
        final long serial;
        CameraDevice device;
        CameraCaptureSession session;
        ImageReader reader;
        CaptureRequest.Builder request;
        CameraCaptureSession.CaptureCallback callback;
        Attempt(long serial) { this.serial = serial; }
    }

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        TextView text = new TextView(this);
        text.setText("Virtual camera acceptance probe\nKeep this activity foreground.\nResults: app-private status.json");
        setContentView(text);
        manager = getSystemService(CameraManager.class);
        output = new AtomicFile(new File(getFilesDir(), "status.json"));
        thread = new HandlerThread("VCamE2ECamera"); thread.start();
        handler = new Handler(thread.getLooper());
        handler.post(() -> {
            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                fail("ordinary CAMERA permission not granted");
            }
            control(getIntent());
            tick.run();
        });
    }

    @Override public void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handler.post(() -> control(intent));
    }

    private void control(Intent intent) {
        try {
            probeOnly=intent.getBooleanExtra("probe_only",probeOnly);
            for (Slot slot : slots) {
                int width=intent.getIntExtra("width"+slot.id,slot.width),height=intent.getIntExtra("height"+slot.id,slot.height);
                int fps=intent.getIntExtra("fps"+slot.id,slot.fps);
                boolean rateChanged=fps!=slot.fps;
                if(width!=slot.width||height!=slot.height)close(slot);
                slot.width=width;slot.height=height;slot.fps=fps;
                if(rateChanged&&slot.attempt!=null&&slot.attempt.session!=null)repeat(slot,slot.attempt);
                int run = intent.getIntExtra("run" + slot.id, slot.expectedRun);
                if (run <= 0) throw new IllegalArgumentException("missing expected run for " + slot.id);
                if (slot.expectedRun != run) {
                    slot.expectedRun = run;
                    slot.sequence = 0; slot.generation = 0; slot.timestamp = 0;
                    slot.minGeneration = 1;
                }
                if (intent.getBooleanExtra("reopen" + slot.id, false)) {
                    slot.minGeneration = slot.generation + 1;
                    close(slot);
                }
            }
            command = intent.getIntExtra("command", command);
        } catch (Exception e) { fail("control: " + e); }
        save();
    }

    private final Runnable tick = new Runnable() {
        @Override public void run() {
            if (stopping) return;
            try {
                inventory = new HashSet<>(Arrays.asList(manager.getCameraIdList()));
                for (Slot slot : slots) {
                    // Do NOT synthesize disconnect by closing on inventory removal.
                    // The harness must observe a genuine framework StateCallback.
                    if (!probeOnly && inventory.contains(slot.id) && slot.attempt == null && failure.isEmpty()) open(slot);
                }
            } catch (Exception e) { fail("inventory: " + e); }
            save();
            handler.postDelayed(this, 250);
        }
    };

    private void open(Slot slot) throws Exception {
        CameraCharacteristics characteristics = manager.getCameraCharacteristics(slot.id);
        Size[] sizes = characteristics.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
                .getOutputSizes(ImageFormat.YUV_420_888);
        if (sizes == null || !Arrays.asList(sizes).contains(new Size(slot.width, slot.height))) {
            throw new IllegalStateException(slot.id + " lacks requested advertised YUV profile " + slot.width + "x" + slot.height);
        }
        Attempt attempt = new Attempt(++slot.openSerial);
        slot.attempt = attempt;
        attempt.reader = ImageReader.newInstance(slot.width, slot.height, ImageFormat.YUV_420_888, 4);
        attempt.reader.setOnImageAvailableListener(reader -> read(slot, attempt, reader), handler);
        try {
            manager.openCamera(slot.id, new CameraDevice.StateCallback() {
                @Override public void onOpened(CameraDevice device) {
                    if (slot.attempt != attempt || stopping) { device.close(); return; }
                    attempt.device = device;
                    slot.opens++;
                    try {
                        device.createCaptureSession(Arrays.asList(attempt.reader.getSurface()),
                                new CameraCaptureSession.StateCallback() {
                            @Override public void onConfigured(CameraCaptureSession session) {
                                if (slot.attempt != attempt || stopping) { session.close(); return; }
                                attempt.session = session;
                                try {
                                    repeat(slot,attempt);
                                } catch (Exception e) { fail(slot.id + " repeating: " + e); close(slot); }
                            }
                            @Override public void onConfigureFailed(CameraCaptureSession session) {
                                if (slot.attempt == attempt) { fail(slot.id + " configuration rejected"); close(slot); }
                            }
                        }, handler);
                    } catch (Exception e) { fail(slot.id + " session: " + e); close(slot); }
                }
                @Override public void onDisconnected(CameraDevice device) {
                    if (slot.attempt == attempt) { slot.disconnects++; close(slot); }
                    device.close();
                }
                @Override public void onError(CameraDevice device, int error) {
                    if (slot.attempt == attempt) {
                        slot.deviceErrors++; slot.lastDeviceError = error; close(slot);
                    }
                    device.close();
                }
            }, handler);
        } catch (Exception e) { close(slot); throw e; }
    }

    private void repeat(Slot slot,Attempt attempt) throws Exception {
                                    CaptureRequest.Builder request = attempt.device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
                                    request.addTarget(attempt.reader.getSurface());
                                    // fps=0 preserves the actual Camera2 default-template range.
                                    if(slot.fps>0)request.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE,new android.util.Range<Integer>(slot.fps,slot.fps));
                                    attempt.callback = new CameraCaptureSession.CaptureCallback() {
                                        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest r,
                                                TotalCaptureResult result) {
                                            if (slot.attempt == attempt) {
                                                slot.completed++;
                                                Long duration=result.get(android.hardware.camera2.CaptureResult.SENSOR_FRAME_DURATION);
                                                if(duration!=null)slot.frameDuration=duration;
                                            }
                                        }
                                        @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest r,
                                                CaptureFailure error) {
                                            if (slot.attempt == attempt) slot.captureFailures++;
                                        }
                                        @Override public void onCaptureBufferLost(CameraCaptureSession s, CaptureRequest r,
                                                android.view.Surface target, long frame) {
                                            if (slot.attempt == attempt) slot.bufferLost++;
                                        }
                                    };
                                    attempt.request=request;
                                    attempt.session.setRepeatingRequest(request.build(),attempt.callback,handler);
    }

    private void read(Slot slot, Attempt attempt, ImageReader reader) {
        if (slot.attempt != attempt) return;
        try (Image image = reader.acquireNextImage()) {
            if (image == null) return;
            Image.Plane plane = image.getPlanes()[0];
            FrameIdentity id = FrameIdentity.decode(plane.getBuffer(), image.getWidth(), image.getHeight(),
                    plane.getRowStride(), plane.getPixelStride());
            if (id.producer != slot.producer || id.run != slot.expectedRun) {
                throw new IllegalStateException("wrong producer/run " + id.producer + "/" + id.run
                        + " expected " + slot.producer + "/" + slot.expectedRun);
            }
            if (id.sequence <= slot.sequence || id.generation < Math.max(slot.generation, slot.minGeneration)) {
                throw new IllegalStateException("duplicate/regressing frame or configuration "
                        + id.sequence + "/" + id.generation + " after " + slot.sequence + "/" + slot.generation);
            }
            if (image.getTimestamp() <= slot.timestamp) throw new IllegalStateException("non-monotonic image timestamp");
            slot.sequence = id.sequence; slot.generation = id.generation;
            slot.timestamp = image.getTimestamp(); slot.flipped = id.flipped;
            slot.images++;
        } catch (Exception e) { fail(slot.id + " pixels: " + e); }
    }

    private void close(Slot slot) {
        Attempt attempt = slot.attempt;
        if (attempt == null) return;
        slot.attempt = null;
        if (attempt.session != null) attempt.session.close();
        if (attempt.device != null) attempt.device.close();
        if (attempt.reader != null) attempt.reader.close();
    }

    private void fail(String message) {
        if (failure.isEmpty()) { failure = message; Log.e(TAG, "FAIL " + message); }
    }

    private void save() {
        FileOutputStream stream = null;
        try {
            JSONObject result = new JSONObject();
            result.put("schema", 1); result.put("command", command);
            result.put("elapsed_ms", SystemClock.elapsedRealtime());
            result.put("failure", failure); result.put("inventory", new JSONArray(inventory));
            JSONObject cameras = new JSONObject();
            for (Slot slot : slots) {
                JSONObject stats = new JSONObject();
                stats.put("width",slot.width);stats.put("height",slot.height);stats.put("requested_fps",slot.fps);
                if(inventory.contains(slot.id)) {
                    try {
                    CameraCharacteristics c=manager.getCameraCharacteristics(slot.id);
                    android.hardware.camera2.params.StreamConfigurationMap map=c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                    JSONArray modes=new JSONArray();
                    Size[] declaredYuv=map==null?null:map.getOutputSizes(ImageFormat.YUV_420_888);
                    if(declaredYuv!=null)for(Size size:declaredYuv) {
                        JSONObject mode=new JSONObject();mode.put("width",size.getWidth());mode.put("height",size.getHeight());
                        mode.put("min_duration_ns",map.getOutputMinFrameDuration(ImageFormat.YUV_420_888,size));modes.put(mode);
                    }
                    stats.put("advertised_yuv",modes);
                    JSONArray rates=new JSONArray();for(android.util.Range<Integer> rate:c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES))rates.put(new JSONArray(new int[]{rate.getLower(),rate.getUpper()}));
                    stats.put("advertised_fps",rates);
                    stats.put("jpeg",map!=null&&map.getOutputSizes(ImageFormat.JPEG)!=null);
                    } catch(android.hardware.camera2.CameraAccessException | IllegalArgumentException removed) { stats.put("metadata_pending",true); }
                }
                stats.put("expected_run", slot.expectedRun); stats.put("producer", slot.producer);
                stats.put("images", slot.images); stats.put("completed", slot.completed);stats.put("frame_duration_ns",slot.frameDuration);
                stats.put("capture_failures", slot.captureFailures); stats.put("buffer_lost", slot.bufferLost);
                stats.put("opens", slot.opens); stats.put("disconnects", slot.disconnects);
                stats.put("device_errors", slot.deviceErrors); stats.put("last_device_error", slot.lastDeviceError);
                stats.put("sequence", slot.sequence); stats.put("generation", slot.generation);
                stats.put("minimum_generation", slot.minGeneration);
                stats.put("flipped", slot.flipped); stats.put("timestamp", slot.timestamp);
                stats.put("active", slot.attempt != null && slot.attempt.session != null);
                cameras.put(slot.id, stats);
            }
            result.put("cameras", cameras);
            stream = output.startWrite();
            stream.write(result.toString().getBytes(StandardCharsets.UTF_8));
            output.finishWrite(stream);
        } catch (Exception e) {
            if (stream != null) output.failWrite(stream);
            fail("status write: " + e);
        }
    }

    @Override public void onDestroy() {
        handler.post(() -> {
            stopping = true;
            for (Slot slot : slots) close(slot);
            handler.removeCallbacksAndMessages(null);
            thread.quitSafely();
        });
        super.onDestroy();
    }
}
