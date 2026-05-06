#include "zed_webcam_send.hpp"
#include "zed_webcam_common.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

void run_send_mode(const std::string &server, int port, int width, int height,
                   int fps, int bitrate_bps, bool hevc) {
  {
    std::lock_guard<std::mutex> lock(config_mutex);
    current_camera_config = CameraRequestData();
    current_camera_config.width = width;
    current_camera_config.height = height;
    current_camera_config.fps = fps;
    current_camera_config.bitrate = bitrate_bps;
    current_camera_config.enableMvHevc = hevc ? 1 : 0;
    current_camera_config.renderMode = 2;
    current_camera_config.port = port;
    current_camera_config.camera = "ZED";
    current_camera_config.ip = server;
  }

  send_to_server = server;
  send_to_port = port;

  std::cout << "[send] 直连推流目标 TCP " << send_to_server << ":" << send_to_port
            << "  编码: " << width << "x" << height << " @ " << fps << "fps"
            << " bitrate(bps)=" << bitrate_bps
            << (hevc ? " HEVC" : " H.264") << std::endl;

  startStreamingThread();

  while (!stop_requested.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  stopStreamingThread();
}
