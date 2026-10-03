// SPDX-License-Identifier: GPL-2.0-only
#include <openvr_driver.h>
#include <dlfcn.h>
#include <cstring>
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) { std::cerr << dlerror() << '\n'; return 1; }
    auto factory = reinterpret_cast<void *(*)(const char *, int *)>(dlsym(library, "HmdDriverFactory"));
    if (!factory) return 2;
    int error = -1;
    if (factory("unsupported", &error) || error != vr::VRInitError_Init_InterfaceNotFound) return 3;
    auto provider = static_cast<vr::IServerTrackedDeviceProvider *>(factory(vr::IServerTrackedDeviceProvider_Version, &error));
    if (!provider || error != vr::VRInitError_None) return 4;
    bool found = false;
    for (auto p = provider->GetInterfaceVersions(); *p; ++p)
        found |= std::strcmp(*p, vr::IVRDisplayComponent_Version) == 0;
    dlclose(library);
    return found ? 0 : 5;
}
