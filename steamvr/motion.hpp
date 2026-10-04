// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <sodium.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <algorithm>

namespace ndvr {
inline uint32_t u32(const unsigned char *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t u64(const unsigned char *p) { return uint64_t(u32(p)) << 32 | u32(p + 4); }
struct Motion {
    std::array<double, 4> q{0, 0, 0, 1};
    std::array<double, 3> rate{};
    uint64_t session = 0, sample = 0;
    uint32_t sequence = 0;
    double received = 0, sourceAge = 0;
    std::array<double, 3> position{}, velocity{};
    bool spatial = false, orientationValid = true, positionValid = false;
    uint32_t trackingQuality = 0, generation = 0;
};
class MotionState {
    std::deque<uint64_t> retired;
public:
    Motion latest;
    bool accept(const unsigned char *p, size_t size, const unsigned char *key, double now) {
        const bool spatial = size == 160;
        const size_t body = spatial ? 128 : 96;
        if ((size != 128 && !spatial) || !std::isfinite(now) ||
            crypto_auth_hmacsha256_verify(p + body, p, body, key) != 0 ||
            std::memcmp(p, spatial ? "NDM2\0\2" : "NDM1\0\1", 6) ||
            p[6] || (p[7] & ~(spatial ? 7 : 1)) || !u64(p + 12)) return false;
        std::array<double, 6> spatialValues{};
        if (spatial) {
            for (size_t i = 0; i < spatialValues.size(); ++i) {
                uint32_t bits = u32(p + 96 + i * 4); float value;
                std::memcpy(&value, &bits, 4);
                if (!std::isfinite(value) || std::abs(value) > (i < 3 ? 100.0 : 20.0)) return false;
                spatialValues[i] = value;
            }
            // Only ARKit normal tracking may claim a measured position.
            if (u32(p + 120) > 2 || u32(p + 124) != 0 ||
                ((p[7] & 2) && (!(p[7] & 4) || u32(p + 120) != 2))) return false;
        }
        std::array<double, 14> f{};
        for (size_t i = 0; i < f.size(); ++i) {
            uint32_t bits = u32(p + 36 + i * 4); float value;
            std::memcpy(&value, &bits, 4); f[i] = value;
            if (!std::isfinite(value)) return false;
        }
        double norm = 0, rawNorm = 0;
        for (int i = 0; i < 4; ++i) { norm += f[i]*f[i]; rawNorm += f[i+10]*f[i+10]; }
        if (norm <= .8 || norm >= 1.2 || rawNorm <= .8 || rawNorm >= 1.2) return false;
        uint64_t session = u64(p + 12), sample = u64(p + 20);
        uint32_t seq = u32(p + 8);
        if (latest.session) {
            if (session == latest.session) {
                uint32_t step = seq - latest.sequence;
                if (!step || step >= 0x80000000u || sample < latest.sample) return false;
            } else {
                if (now - latest.received < 1 || std::find(retired.begin(), retired.end(), session) != retired.end()) return false;
                retired.push_back(latest.session);
                if (retired.size() > 64) retired.pop_front();
            }
        }
        latest.session = session; latest.sequence = seq; latest.sample = sample; latest.received = now;
        latest.spatial = spatial;
        latest.orientationValid = !spatial || (p[7] & 4);
        latest.positionValid = spatial && (p[7] & 2);
        latest.trackingQuality = spatial ? u32(p + 120) : 0;
        latest.generation = u32(p + 92);
        uint64_t sent = u64(p + 28);
        // These two timestamps are both on the phone. Never subtract a phone
        // timestamp from a Linux clock. Older clients used another uptime clock;
        // ignore implausible differences rather than rejecting their packets.
        latest.sourceAge = sent >= sample && sent - sample <= 1000000000ull
            ? double(sent - sample) / 1e9 : 0.0;
        for (int i = 0; i < 3; ++i) {
            latest.position[i] = spatialValues[i];
            latest.velocity[i] = spatialValues[i + 3];
        }
        for (int i = 0; i < 4; ++i) latest.q[i] = f[i] / std::sqrt(norm);
        for (int i = 0; i < 3; ++i) latest.rate[i] = f[i+4];
        return true;
    }
    bool fresh(double now) const {
        return latest.session && now >= latest.received &&
               now - latest.received + latest.sourceAge <= .25;
    }
    bool connected(double now) const {
        return latest.session && now >= latest.received && now - latest.received <= 2.0;
    }
    bool valid(double now) const {
        return fresh(now) && latest.orientationValid && (!latest.spatial || latest.positionValid);
    }
};
}
