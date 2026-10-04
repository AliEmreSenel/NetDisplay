// SPDX-License-Identifier: GPL-2.0-only
#include "motion.hpp"
#include <cstdlib>
#include <iostream>
#include <fstream>
#define CHECK(x) do { if (!(x)) { std::cerr << "Failed line " << __LINE__ << ": " #x "\n"; std::exit(1); } } while (0)
void put(unsigned char *p, uint32_t v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }
int main(int argc, char **argv) {
    CHECK(sodium_init() >= 0);
    std::array<unsigned char, 32> key{};
    std::array<unsigned char, 128> p{};
    std::memcpy(p.data(), "NDM1\0\1", 6); p[19]=1; put(p.data()+8, 1);
    put(p.data()+48, 0x3f800000); put(p.data()+88, 0x3f800000);
    auto sign = [&] { crypto_auth_hmacsha256(p.data()+96, p.data(), 96, key.data()); };
    ndvr::MotionState state; sign();
    CHECK(state.accept(p.data(), 128, key.data(), 1)); CHECK(state.fresh(1.2)); CHECK(!state.fresh(1.3));
    CHECK(!state.accept(p.data(), 128, key.data(), 2)); // replay
    put(p.data()+8, 2); CHECK(!state.accept(p.data(), 128, key.data(), 2)); // tampered
    sign(); CHECK(state.accept(p.data(), 128, key.data(), 2));
    CHECK(!state.accept(p.data(), 127, key.data(), 3));
    p[19]=2; sign(); CHECK(!state.accept(p.data(), 128, key.data(), 2.5));
    CHECK(state.accept(p.data(), 128, key.data(), 3.1));
    p[19]=1; sign(); CHECK(!state.accept(p.data(), 128, key.data(), 5)); // retired session
    p[19]=2; put(p.data()+8, 3); put(p.data()+36, 0x7fc00000); sign();
    CHECK(!state.accept(p.data(), 128, key.data(), 5)); // NaN
    put(p.data()+36, 0); put(p.data()+48, 0); sign(); CHECK(!state.accept(p.data(), 128, key.data(), 5));
    put(p.data()+48, 0x3f800000); put(p.data()+8, 1); sign(); CHECK(!state.accept(p.data(), 128, key.data(), 5));
    CHECK(state.connected(3.5)); CHECK(!state.connected(6));
    std::array<unsigned char, 160> p2{};
    std::memcpy(p2.data(), "NDM2\0\2", 6); p2[7] = 7; p2[19] = 9;
    put(p2.data()+8, 1); put(p2.data()+48, 0x3f800000); put(p2.data()+88, 0x3f800000);
    put(p2.data()+96, 0x3e800000); put(p2.data()+100, 0xbe000000); put(p2.data()+104, 0xbf000000);
    put(p2.data()+120, 2);
    auto sign2 = [&] { crypto_auth_hmacsha256(p2.data()+128, p2.data(), 128, key.data()); };
    ndvr::MotionState spatial; sign2();
    CHECK(spatial.accept(p2.data(), p2.size(), key.data(), 10));
    CHECK(spatial.latest.spatial && spatial.valid(10.1) && spatial.connected(11));
    CHECK(spatial.latest.position[0] == .25 && spatial.latest.position[1] == -.125 && spatial.latest.position[2] == -.5);
    CHECK(!spatial.accept(p2.data(), p2.size(), key.data(), 10.1));
    put(p2.data()+8, 2); p2[7] = 1; put(p2.data()+120, 1); sign2();
    CHECK(spatial.accept(p2.data(), p2.size(), key.data(), 10.2));
    CHECK(!spatial.valid(10.21) && spatial.connected(10.21)); // tracking loss, not unplug
    put(p2.data()+8, 3); p2[7] = 7; sign2();
    CHECK(!spatial.accept(p2.data(), p2.size(), key.data(), 10.3)); // limited cannot claim position
    put(p2.data()+120, 2); put(p2.data()+96, 0x7fc00000); sign2();
    CHECK(!spatial.accept(p2.data(), p2.size(), key.data(), 10.3));
    put(p2.data()+96, 0); put(p2.data()+124, 1); sign2();
    CHECK(!spatial.accept(p2.data(), p2.size(), key.data(), 10.3)); // reserved
    put(p2.data()+124, 0); put(p2.data()+24, 1000000); put(p2.data()+32, 201000000); sign2();
    CHECK(spatial.accept(p2.data(), p2.size(), key.data(), 10.3));
    CHECK(spatial.valid(10.31) && !spatial.valid(10.4)); // phone processing age
    CHECK(!spatial.accept(p2.data(), 159, key.data(), 12));
    if (argc >= 2) {
        std::ifstream file(argv[1], std::ios::binary);
        std::array<unsigned char, 128> fixture{};
        CHECK(bool(file.read(reinterpret_cast<char *>(fixture.data()), fixture.size())));
        CHECK(file.peek() == std::char_traits<char>::eof());
        for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<unsigned char>(i);
        ndvr::MotionState interop;
        CHECK(interop.accept(fixture.data(), fixture.size(), key.data(), 1));
        CHECK(interop.latest.session == 0x0102030405060708ull && interop.latest.sequence == 7);
        CHECK(interop.latest.sample == 123 && interop.latest.q[3] == 1);
        CHECK(interop.latest.rate[0] == 1 && interop.latest.rate[1] == 2 && interop.latest.rate[2] == 3);
    }
    if (argc >= 3) {
        std::ifstream file(argv[2], std::ios::binary); std::array<unsigned char, 160> fixture{};
        CHECK(bool(file.read(reinterpret_cast<char *>(fixture.data()), fixture.size())));
        CHECK(file.peek() == std::char_traits<char>::eof());
        ndvr::MotionState interop;
        CHECK(interop.accept(fixture.data(), fixture.size(), key.data(), 1));
        CHECK(interop.latest.spatial && interop.valid(1.1));
        CHECK(interop.latest.position[0] == .25 && interop.latest.position[1] == -.125 && interop.latest.position[2] == -.5);
        CHECK(interop.latest.generation == 2 && interop.latest.trackingQuality == 2);
    }
    std::cout << "SteamVR motion validation passed\n";
}
