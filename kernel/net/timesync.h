#pragma once

namespace net {

// Keeps the clock right whenever there is a network: reads the Date header of a
// small HTTPS response after the network comes up, then every few hours.
void timesync_init();

}  // namespace net
