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
    if (argc == 2) {
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
    std::cout << "SteamVR motion validation passed\n";
}
