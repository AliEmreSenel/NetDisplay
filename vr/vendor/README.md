# Capture layer provenance

Vendored from https://github.com/alvr-org/ALVR at `9f11839431f95f0df6764790d9a16f8e308a0d79`.
The capture layer derives from Arm Vulkan WSI (MIT); retain all source notices,
`capture/LICENSE`, `ALVR-LICENSE`, and picojson’s embedded license.

NetDisplay adaptations: independent layer name, IPC socket and configuration
environment; C++ entry point instead of Rust; no stack scanning for pose (the
NetDisplay HMD driver supplies tracking). The Wayland DRM lease shim is scoped
to the SteamVR compositor by our reversible wrapper.
