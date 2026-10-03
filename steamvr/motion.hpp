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
    double received = 0;
};
class MotionState {
    std::deque<uint64_t> retired;
public:
    Motion latest;
    bool accept(const unsigned char *p, size_t size, const unsigned char *key, double now) {
        if (size != 128 || crypto_auth_hmacsha256_verify(p + 96, p, 96, key) != 0 ||
            std::memcmp(p, "NDM1\0\1", 6) || p[6] || (p[7] & ~1) || !u64(p + 12)) return false;
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
        for (int i = 0; i < 4; ++i) latest.q[i] = f[i] / std::sqrt(norm);
        for (int i = 0; i < 3; ++i) latest.rate[i] = f[i+4];
        return true;
    }
    bool fresh(double now) const { return latest.session && now - latest.received <= .25; }
};
}
