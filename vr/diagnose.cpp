// SPDX-License-Identifier: GPL-2.0-only
// Live, read-only runtime probe. Does not bind the motion UDP port or alter setup.
#include <openvr.h>
#include <dlfcn.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "libopenvr_api.so";
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "Cannot load %s: %s\nPass the OpenVR SDK lib/linux64/libopenvr_api.so path.\n", path, dlerror()); return 2; }
    auto init = reinterpret_cast<uint32_t (*)(vr::EVRInitError *, vr::EVRApplicationType, const char *)>(dlsym(library, "VR_InitInternal2"));
    auto get = reinterpret_cast<void *(*)(const char *, vr::EVRInitError *)>(dlsym(library, "VR_GetGenericInterface"));
    auto shutdown = reinterpret_cast<void (*)()>(dlsym(library, "VR_ShutdownInternal"));
    if (!init || !get || !shutdown) { fputs("Not an OpenVR API library\n", stderr); dlclose(library); return 2; }
    vr::EVRInitError error = vr::VRInitError_None;
    init(&error, vr::VRApplication_Background, nullptr);
    if (error != vr::VRInitError_None) { fprintf(stderr, "OpenVR init error %d; start SteamVR and connect the phone first.\n", error); dlclose(library); return 1; }
    auto system = static_cast<vr::IVRSystem *>(get(vr::IVRSystem_Version, &error));
    if (!system || error != vr::VRInitError_None) { fprintf(stderr, "IVRSystem version error %d\n", error); shutdown(); dlclose(library); return 1; }
    vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
    char name[256]{};
    system->GetStringTrackedDeviceProperty(0, vr::Prop_TrackingSystemName_String, name, sizeof(name), &pe);
    printf("trackingSystem=%s propertyError=%d\n", name, pe);
    auto universe = system->GetUint64TrackedDeviceProperty(0, vr::Prop_CurrentUniverseId_Uint64, &pe);
    printf("universe=%llu propertyError=%d (NetDisplay expects 20036)\n", (unsigned long long)universe, pe);
    char chaperone[8192]{};
    auto bytes = system->GetStringTrackedDeviceProperty(0, vr::Prop_DriverProvidedChaperoneJson_String, chaperone, sizeof(chaperone), &pe);
    printf("providedChaperoneBytes=%u propertyError=%d\n", bytes, pe);
    for (int tick = 0; tick < 6; ++tick) {
        const vr::ETrackingUniverseOrigin spaces[] = {vr::TrackingUniverseRawAndUncalibrated, vr::TrackingUniverseStanding, vr::TrackingUniverseSeated};
        const char *names[] = {"raw", "standing", "seated"};
        for (int i = 0; i < 3; ++i) {
            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount]{};
            system->GetDeviceToAbsoluteTrackingPose(spaces[i], 0, poses, vr::k_unMaxTrackedDeviceCount);
            const auto &p = poses[0];
            printf("%s connected=%d valid=%d result=%d xyz=%.3f,%.3f,%.3f\n", names[i],
                p.bDeviceIsConnected, p.bPoseIsValid, p.eTrackingResult,
                p.mDeviceToAbsoluteTracking.m[0][3], p.mDeviceToAbsoluteTracking.m[1][3], p.mDeviceToAbsoluteTracking.m[2][3]);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    auto ch = static_cast<vr::IVRChaperone *>(get(vr::IVRChaperone_Version, &error));
    if (ch && error == vr::VRInitError_None) {
        float width = 0, depth = 0; bool area = ch->GetPlayAreaSize(&width, &depth);
        printf("chaperoneCalibrationState=%d playAreaValid=%d size=%.2fx%.2f\n", ch->GetCalibrationState(), area, width, depth);
    }
    puts("Raw valid but standing invalid: origin/calibration integration. Both invalid: inspect NDVR tracking freshness.\nBoth valid/result=200 but wizard stuck: wizard/UI issue, not absent motion. Use driver-provided stationary setup; do not add fake base stations.");
    shutdown(); dlclose(library); return 0;
}
