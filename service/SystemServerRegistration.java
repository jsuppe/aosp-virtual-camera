/*
 * SystemServer Registration for VirtualCameraService
 * 
 * Add this to: frameworks/base/services/java/com/android/server/SystemServer.java
 */

// ============ STEP 1: Add import ============
// At the top of SystemServer.java, add:
import com.android.server.camera.virtual.VirtualCameraService;


// ============ STEP 2: Add to startOtherServices() ============
// In the startOtherServices() method, find a suitable location
// (after CameraService, or in the "others" section) and add:

/**
 * Starts the Virtual Camera Service.
 * 
 * Add this block inside startOtherServices(), around line ~1500-2000
 * (near other camera/media services)
 */
private void startVirtualCameraService() {
    // In SystemServer.java, inside startOtherServices():
    
    t.traceBegin("StartVirtualCameraService");
    try {
        mSystemServiceManager.startService(VirtualCameraService.Lifecycle.class);
    } catch (Throwable e) {
        reportWtf("starting VirtualCameraService", e);
    }
    t.traceEnd();
}


// ============ STEP 3: Create Lifecycle wrapper ============
// Add this class to VirtualCameraService.java:

/**
 * Lifecycle wrapper for SystemServiceManager.
 * Add this as an inner class in VirtualCameraService.java
 */
public static class Lifecycle extends SystemService {
    private VirtualCameraService mService;
    
    public Lifecycle(Context context) {
        super(context);
    }
    
    @Override
    public void onStart() {
        mService = new VirtualCameraService(getContext());
        publishBinderService(SERVICE_NAME, mService);
        publishBinderService(MANAGER_SERVICE_NAME, mService.getManagerBinder());
    }
    
    @Override
    public void onBootPhase(int phase) {
        if (phase == SystemService.PHASE_SYSTEM_SERVICES_READY) {
            // Service dependencies are ready
            mService.onSystemServicesReady();
        } else if (phase == SystemService.PHASE_BOOT_COMPLETED) {
            // Boot complete, can start accepting connections
            mService.onBootCompleted();
        }
    }
    
    @Override
    public void onUserStarting(@NonNull TargetUser user) {
        // Handle user-specific initialization if needed
    }
}
