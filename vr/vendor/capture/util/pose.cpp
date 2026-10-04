#include "pose.hpp"
const TrackedDevicePose_t &find_pose_in_call_stack() { static const TrackedDevicePose_t pose{}; return pose; }
