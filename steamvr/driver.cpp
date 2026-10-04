// SPDX-License-Identifier: GPL-2.0-only
#include <openvr_driver.h>
#include "motion.hpp"
#include "tracking_space.hpp"
#include <atomic>
#include <thread>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

namespace {
#ifdef NETDISPLAY_VR_DIRECT
constexpr const char *section = "driver_netdisplay_vr";
#else
constexpr const char *section = "driver_netdisplay";
#endif
double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::string setting(const char *key) {
    char value[1024]{}; vr::VRSettings()->GetString(section, key, value, sizeof(value)); return value;
}
class HMD final : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent {
    std::mutex mutex;
    ndvr::MotionState motion;
    std::array<unsigned char, 32> key{};
    int fd = -1;
    in_addr peer{};
    vr::TrackedDeviceIndex_t index = vr::k_unTrackedDeviceIndexInvalid;
    uint32_t width = 1920, height = 1080;
    int32_t x = 0, y = 0;
    float fps = 60, ipd = .064f, fov = 90;
    bool standby = false;
    float headHeight = 1.6f;
    std::atomic_bool running{false};
    std::thread receiver;
    bool lastSpatial = false;
    uint64_t rx = 0, rxPeer = 0, rxOk = 0;
    double lastLog = 0;
    in_addr lastFrom{};
public:
    ~HMD() { stop(); }
    bool start() {
        if (sodium_init() < 0) return false;
        width = vr::VRSettings()->GetInt32(section, "width");
        height = vr::VRSettings()->GetInt32(section, "height");
        x = vr::VRSettings()->GetInt32(section, "windowX"); y = vr::VRSettings()->GetInt32(section, "windowY");
        fps = vr::VRSettings()->GetFloat(section, "frequency");
        ipd = vr::VRSettings()->GetFloat(section, "ipd"); fov = vr::VRSettings()->GetFloat(section, "verticalFov");
        int port = vr::VRSettings()->GetInt32(section, "motionPort");
        vr::EVRSettingsError settingError = vr::VRSettingsError_None;
        float configuredHeight = vr::VRSettings()->GetFloat(section, "headHeight", &settingError);
        if (settingError == vr::VRSettingsError_None) headHeight = configuredHeight;
        if (!std::isfinite(headHeight) || headHeight < .3f || headHeight > 2.5f) return false;
        if (width < 2 || width > 4096 || width % 2 || height < 2 || height > 2160 ||
            !std::isfinite(fps) || fps < 30 || fps > 120 || !std::isfinite(ipd) || ipd < .04 || ipd > .09 ||
            !std::isfinite(fov) || fov < 40 || fov > 130 || port < 1 || port > 65535) return false;
        std::ifstream file(setting("tokenFile")); std::string token; file >> token;
        if (token.size() != 64 || sodium_hex2bin(key.data(), key.size(), token.data(), token.size(), nullptr, nullptr, nullptr)) return false;
        sodium_memzero(token.data(), token.size());
        sockaddr_in local{}; local.sin_family = AF_INET; local.sin_port = htons(port);
        if (inet_pton(AF_INET, setting("bindAddress").c_str(), &local.sin_addr) != 1 ||
            inet_pton(AF_INET, setting("peerAddress").c_str(), &peer) != 1) return false;
        fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0 || bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) < 0) { stop(); return false; }
        int buffer = 32 * 1024;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
        motion = {}; standby = false; lastSpatial = false; running = true;
        try {
            receiver = std::thread([this] {
                while (running) {
                    pollfd ready{fd, POLLIN, 0};
                    (void)poll(&ready, 1, 8);
                    if (running) frame();
                }
            });
        } catch (...) { running = false; stop(); return false; }
        return true;
    }
    void stop() {
        running = false;
        if (receiver.joinable()) receiver.join();
        std::lock_guard<std::mutex> lock(mutex);
        if (fd >= 0) close(fd);
        fd = -1; index = vr::k_unTrackedDeviceIndexInvalid; motion = {}; sodium_memzero(key.data(), key.size());
    }
    vr::EVRInitError Activate(uint32_t id) override {
        auto props = vr::VRProperties(); auto c = props->TrackedDeviceToPropertyContainer(id);
        props->SetStringProperty(c, vr::Prop_TrackingSystemName_String,
#ifdef NETDISPLAY_VR_DIRECT
                                 "netdisplay_vr"
#else
                                 "netdisplay"
#endif
        );
        props->SetStringProperty(c, vr::Prop_ModelNumber_String, "NetDisplay iPhone");
        props->SetInt32Property(c, vr::Prop_DeviceClass_Int32, vr::TrackedDeviceClass_HMD);
        props->SetInt32Property(c, vr::Prop_ExpectedTrackingReferenceCount_Int32, 0);
        props->SetInt32Property(c, vr::Prop_ExpectedControllerCount_Int32, 0);
        props->SetInt32Property(c, vr::Prop_HmdTrackingStyle_Int32, vr::HmdTrackingStyle_Unknown);
        props->SetBoolProperty(c, vr::Prop_NeverTracked_Bool, false);
        props->SetBoolProperty(c, vr::Prop_HasDisplayComponent_Bool, true);
        props->SetStringProperty(c, vr::Prop_SerialNumber_String, "netdisplay-ios-hmd");
        props->SetStringProperty(c, vr::Prop_ManufacturerName_String, "NetDisplay");
        props->SetStringProperty(c, vr::Prop_RenderModelName_String, "generic_hmd");
        props->SetFloatProperty(c, vr::Prop_DisplayFrequency_Float, fps);
        props->SetFloatProperty(c, vr::Prop_UserIpdMeters_Float, ipd);
        props->SetFloatProperty(c, vr::Prop_SecondsFromVsyncToPhotons_Float, 0);
        const auto chaperone = ndvr::stationaryChaperone(headHeight);
        auto chaperoneError = props->SetStringProperty(c, vr::Prop_DriverProvidedChaperoneJson_String, chaperone.c_str());
        props->SetUint64Property(c, vr::Prop_CurrentUniverseId_Uint64, ndvr::universeId);
        char setupLog[256];
        snprintf(setupLog, sizeof(setupLog), "NDVR stationary chaperone universe=%u height=%.3f propertyError=%d (no measured room bounds)\n",
                 ndvr::universeId, headHeight, int(chaperoneError));
        vr::VRDriverLog()->Log(setupLog);
        props->SetBoolProperty(c, vr::Prop_IsOnDesktop_Bool, IsDisplayOnDesktop());
#ifdef NETDISPLAY_VR_DIRECT
        vr::VRSettings()->SetBool("steamvr", "enableLinuxVulkanAsync", false);
        vr::VRSettings()->SetBool("steamvr", "disableAsyncReprojection", true);
#endif
        props->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, true);
        props->SetBoolProperty(c, vr::Prop_ContainsProximitySensor_Bool, false);
        props->SetBoolProperty(c, vr::Prop_ReportsTimeSinceVSync_Bool, false);
        vr::HmdMatrix34_t left{}, right{};
        for (int i = 0; i < 3; ++i) left.m[i][i] = right.m[i][i] = 1;
        left.m[0][3] = -ipd / 2; right.m[0][3] = ipd / 2;
        vr::VRServerDriverHost()->SetDisplayEyeToHead(id, left, right);
        { std::lock_guard<std::mutex> lock(mutex); index = id; }
        return vr::VRInitError_None;
    }
    void Deactivate() override { std::lock_guard<std::mutex> lock(mutex); index = vr::k_unTrackedDeviceIndexInvalid; }
    void EnterStandby() override { setStandby(true); }
    void setStandby(bool value) { std::lock_guard<std::mutex> lock(mutex); standby = value; }
    void *GetComponent(const char *name) override {
        return std::strcmp(name, vr::IVRDisplayComponent_Version) == 0 ? static_cast<vr::IVRDisplayComponent *>(this) : nullptr;
    }
    void DebugRequest(const char *, char *response, uint32_t size) override { if (size) response[0] = 0; }
    vr::DriverPose_t GetPose() override {
        std::lock_guard<std::mutex> lock(mutex);
        vr::DriverPose_t p{};
        p.qWorldFromDriverRotation.w = p.qDriverFromHeadRotation.w = 1;
        const auto &m = motion.latest;
        p.qRotation = {m.q[3], m.q[0], m.q[1], m.q[2]};
        const double now = nowSeconds();
        p.vecPosition[1] = headHeight;
        if (m.spatial) {
            for (int i = 0; i < 3; ++i) p.vecPosition[i] += m.position[i];
        }
        // Missing/limited tracking is different from unplugging the device.
        p.deviceIsConnected = motion.connected(now);
        p.poseIsValid = !standby && motion.valid(now);
        p.result = p.poseIsValid ? vr::TrackingResult_Running_OK : vr::TrackingResult_Running_OutOfRange;
        p.willDriftInYaw = !m.spatial; p.shouldApplyHeadModel = false;
        if (p.poseIsValid) {
            // Core Motion rates are in local headset axes. OpenVR expects driver/world axes.
            double qx=m.q[0], qy=m.q[1], qz=m.q[2], qw=m.q[3];
            double a=m.rate[0], b=m.rate[1], c=m.rate[2];
            double tx=2*(qy*c-qz*b), ty=2*(qz*a-qx*c), tz=2*(qx*b-qy*a);
            p.vecAngularVelocity[0]=a+qw*tx+qy*tz-qz*ty;
            p.vecAngularVelocity[1]=b+qw*ty+qz*tx-qx*tz;
            p.vecAngularVelocity[2]=c+qw*tz+qx*ty-qy*tx;
            if (m.spatial) for (int i = 0; i < 3; ++i) p.vecVelocity[i] = m.velocity[i];
            p.poseTimeOffset = -std::min(.25, now - m.received + m.sourceAge);
        }
        return p;
    }
    bool streaming() { std::lock_guard<std::mutex> lock(mutex); return motion.fresh(nowSeconds()); }
    void frame() {
        vr::TrackedDeviceIndex_t current;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = index;
            for (int i = 0; fd >= 0 && i < 512; ++i) {
                unsigned char packet[161]; sockaddr_in from{}; socklen_t len = sizeof(from);
                ssize_t n = recvfrom(fd, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&from), &len);
                if (n < 0) break;
                ++rx; lastFrom = from.sin_addr;
                if (from.sin_addr.s_addr == peer.s_addr) {
                    ++rxPeer;
                    if (motion.accept(packet, size_t(n), key.data(), nowSeconds())) ++rxOk;
                }
            }
            double now = nowSeconds();
            if (now - lastLog >= 1) {
                char ip[64] = "0.0.0.0", line[384];
                inet_ntop(AF_INET, &lastFrom, ip, sizeof(ip));
                snprintf(line, sizeof(line), "NDVR rx=%llu peer=%llu ok=%llu fresh=%d age=%.3f last=%s valid=%d mode=%s ar=%u sourceAgeMs=%.2f pos=%.3f,%.3f,%.3f\n",
                    (unsigned long long)rx, (unsigned long long)rxPeer, (unsigned long long)rxOk,
                    motion.fresh(now) ? 1 : 0, now - motion.latest.received, ip,
                    !standby && motion.valid(now), motion.latest.spatial ? "6dof" : "3dof",
                    motion.latest.trackingQuality, motion.latest.sourceAge * 1000,
                    motion.latest.position[0], motion.latest.position[1], motion.latest.position[2]);
                vr::VRDriverLog()->Log(line);
                lastLog = now;
            }
        }
        if (current != vr::k_unTrackedDeviceIndexInvalid) {
            bool spatial;
            { std::lock_guard<std::mutex> lock(mutex); spatial = motion.latest.spatial; }
            if (spatial != lastSpatial) {
                auto props = vr::VRProperties(); auto c = props->TrackedDeviceToPropertyContainer(current);
                props->SetInt32Property(c, vr::Prop_HmdTrackingStyle_Int32,
                    spatial ? vr::HmdTrackingStyle_InsideOutCameras : vr::HmdTrackingStyle_Unknown);
                props->SetBoolProperty(c, vr::Prop_WillDriftInYaw_Bool, !spatial);
                lastSpatial = spatial;
            }
            auto pose = GetPose(); vr::VRServerDriverHost()->TrackedDevicePoseUpdated(current, pose, sizeof(pose));
        }
    }
    void GetWindowBounds(int32_t *px, int32_t *py, uint32_t *w, uint32_t *h) override { *px=x; *py=y; *w=width; *h=height; }
    bool IsDisplayOnDesktop() override {
#ifdef NETDISPLAY_VR_DIRECT
        return false;
#else
        return true;
#endif
    }
    bool IsDisplayRealDisplay() override {
#ifdef NETDISPLAY_VR_DIRECT
        // The capture layer provides SteamVR's Vulkan display and swapchain.
        return true;
#else
        return false;
#endif
    }
    void GetRecommendedRenderTargetSize(uint32_t *w, uint32_t *h) override { *w=width/2; *h=height; }
    void GetEyeOutputViewport(vr::EVREye eye, uint32_t *px, uint32_t *py, uint32_t *w, uint32_t *h) override {
        *px = eye == vr::Eye_Left ? 0 : width/2; *py=0; *w=width/2; *h=height;
    }
    void GetProjectionRaw(vr::EVREye, float *l, float *r, float *t, float *b) override {
        float v = std::tan(fov * 3.14159265f / 360.f); *t=-v; *b=v; *r=v*(width/2.f)/height; *l=-*r;
    }
    vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye, float u, float v) override {
        return {{u,v}, {u,v}, {u,v}}; // optional lens correction is applied exactly once, on iOS
    }
    bool ComputeInverseDistortion(vr::HmdVector2_t *p, vr::EVREye, uint32_t, float u, float v) override {
        p->v[0]=u; p->v[1]=v; return true;
    }
};
class Provider final : public vr::IServerTrackedDeviceProvider {
    HMD hmd;
public:
    vr::EVRInitError Init(vr::IVRDriverContext *context) override {
        VR_INIT_SERVER_DRIVER_CONTEXT(context);
        if (!hmd.start()) {
            vr::VRDriverLog()->Log("NetDisplay: invalid settings/token/peer or cannot bind motion UDP. See steamvr/README.md.");
            hmd.stop(); VR_CLEANUP_SERVER_DRIVER_CONTEXT(); return vr::VRInitError_Driver_Failed;
        }
        if (!vr::VRServerDriverHost()->TrackedDeviceAdded("netdisplay-ios-hmd", vr::TrackedDeviceClass_HMD, &hmd)) {
            hmd.stop(); VR_CLEANUP_SERVER_DRIVER_CONTEXT(); return vr::VRInitError_Driver_Failed;
        }
        return vr::VRInitError_None;
    }
    void Cleanup() override { hmd.stop(); VR_CLEANUP_SERVER_DRIVER_CONTEXT(); }
    const char *const *GetInterfaceVersions() override { return vr::k_InterfaceVersions; }
    void RunFrame() override {} // Tracking has its own receive/publish thread.
    bool ShouldBlockStandbyMode() override { return hmd.streaming(); }
    void EnterStandby() override { hmd.setStandby(true); }
    void LeaveStandby() override { hmd.setStandby(false); }
};
Provider provider;
}
extern "C" __attribute__((visibility("default"))) void *HmdDriverFactory(const char *name, int *error) {
    if (std::strcmp(name, vr::IServerTrackedDeviceProvider_Version) == 0) {
        if (error) *error = vr::VRInitError_None;
        return &provider;
    }
    if (error) *error = vr::VRInitError_Init_InterfaceNotFound;
    return nullptr;
}
