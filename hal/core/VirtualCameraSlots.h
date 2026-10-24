/*
 * VirtualCameraSlots - the slot <-> Camera2 device id mapping shared by the
 * provider, the stable HAL and the sessions.
 *
 * Slot s (0..kMaxVirtualCameras-1) is exposed as
 *   device@1.0/virtual_renderer/<kFirstVirtualCameraNumber + s>
 * The platform assigns a slot to each registered producer; slot 0 is what
 * the V1/V2 boundary addresses.
 */
#pragma once

#include <string>

namespace virtualcamera {

constexpr int kMaxVirtualCameras = 4;
constexpr int kFirstVirtualCameraNumber = 100;

inline std::string deviceIdForSlot(int slot) {
    return "device@1.0/virtual_renderer/" + std::to_string(kFirstVirtualCameraNumber + slot);
}

/** -1 if the id is not one of ours. */
inline int slotForDeviceId(const std::string& id) {
    static const std::string prefix = "device@1.0/virtual_renderer/";
    if (id.compare(0, prefix.size(), prefix) != 0) return -1;
    const std::string num = id.substr(prefix.size());
    if (num.empty() || num.find_first_not_of("0123456789") != std::string::npos) return -1;
    const int n = std::stoi(num) - kFirstVirtualCameraNumber;
    return (n >= 0 && n < kMaxVirtualCameras) ? n : -1;
}

inline bool validSlot(int slot) { return slot >= 0 && slot < kMaxVirtualCameras; }

}  // namespace virtualcamera
