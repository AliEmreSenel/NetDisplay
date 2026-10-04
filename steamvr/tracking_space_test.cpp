// SPDX-License-Identifier: GPL-2.0-only
#include "tracking_space.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
int main() {
    for (double bad : {0.0, 3.0, std::numeric_limits<double>::quiet_NaN()}) {
        try { (void)ndvr::stationaryChaperone(bad); return 1; }
        catch (const std::invalid_argument &) {}
    }
    const auto text = ndvr::stationaryChaperone(1.65);
    if (text.find("\"universeID\":20036") == std::string::npos ||
        text.find("\"translation\":[0,1.65,0]") == std::string::npos ||
        text.find("\"collision_bounds\":[]") == std::string::npos) return 1;
    std::cout << text << '\n';
    return 0;
}
