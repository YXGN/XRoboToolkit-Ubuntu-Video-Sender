#ifndef ZED_WEBCAM_SEND_HPP
#define ZED_WEBCAM_SEND_HPP

#include <string>

void run_send_mode(const std::string &server, int port, int width, int height,
                   int fps, int bitrate_bps, bool hevc);

#endif
