// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <cmath>
#include <ctime>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
namespace ndvr {
constexpr unsigned universeId = 0x4e44;
// A stationary origin, NOT a measured room or a collision-safety boundary.
// Raw HMD poses use configured eye height above y=0; AR supplies relative motion.
inline std::string stationaryChaperone(double headHeight) {
    if (!std::isfinite(headHeight) || headHeight < .3 || headHeight > 2.5)
        throw std::invalid_argument("headHeight must be 0.3..2.5 metres");
    std::time_t now = std::time(nullptr); std::tm utc{};
    gmtime_r(&now, &utc);
    char date[32]; std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"json_id\":\"chaperone_info\",\"version\":5,\"time\":\"" << date
        << "\",\"universes\":[{\"universeID\":" << universeId
        << ",\"collision_bounds\":[],\"play_area\":[1.0,1.0],"
           "\"standing\":{\"translation\":[0,0,0],\"yaw\":0},"
           "\"seated\":{\"translation\":[0," << headHeight << ",0],\"yaw\":0}}]}";
    return out.str();
}
}
