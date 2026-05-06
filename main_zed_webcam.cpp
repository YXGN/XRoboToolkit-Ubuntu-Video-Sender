/*
 * main_zed_webcam.cpp — Ubuntu USB 摄像头入口：--listen（Pico 控制协议）或 --send（直连 TCP）。
 */

#include <gst/gst.h>
#include <iostream>
#include <string>

#include "zed_webcam_common.hpp"
#include "zed_webcam_listen.hpp"
#include "zed_webcam_send.hpp"

static void print_usage(const char *argv0) {
  std::cout << "用法: " << argv0 << " (--listen ADDR | --send ...) [选项]\n\n";
  std::cout << "模式（二选一）：\n";
  std::cout << "  --listen IP:PORT     Pico 控制通道：接收 OPEN_CAMERA / CLOSE_CAMERA\n";
  std::cout << "  --send               无控制通道：按 CLI 参数直连 TCP 推 H.264/HEVC\n";
  std::cout << "       --server IP      接收端 IP（send 必选）\n";
  std::cout << "       --port PORT      接收端端口（send 必选）\n";
  std::cout << "       --width W        输出宽（默认 2560）\n";
  std::cout << "       --height H       输出高（默认 720）\n";
  std::cout << "       --fps N          帧率（默认 30）\n";
  std::cout << "       --bitrate BPS    码率 bps（默认 4000000）\n";
  std::cout << "       --hevc           使用 HEVC（默认 H.264）\n";
  std::cout << "\n共用选项：\n";
  std::cout << "  --preview              本机 GStreamer 预览\n";
  std::cout << "  --camera PATH          单目设备（mono-copy）\n";
  std::cout << "  --stereo-camera PATH   双目 SBS，不复制\n";
  std::cout << "  --help\n";
  std::cout << "\n说明：无 ZED SDK；码流格式与 Pico 侧一致（大端 4 字节长度 + 负载）。\n";
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);

  bool preview = false;
  bool listen_mode = false;
  bool send_mode = false;
  std::string listen_addr;
  std::string send_server;
  int send_port = 0;
  int send_w = 2560;
  int send_h = 720;
  int send_fps = 30;
  int send_bitrate = 4000000;
  bool send_hevc = false;
  std::string mono_cam;
  std::string stereo_cam;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_mode = true;
      listen_addr = argv[++i];
    } else if (arg == "--send") {
      send_mode = true;
    } else if (arg == "--server" && i + 1 < argc) {
      send_server = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      send_port = std::stoi(argv[++i]);
    } else if (arg == "--width" && i + 1 < argc) {
      send_w = std::stoi(argv[++i]);
    } else if (arg == "--height" && i + 1 < argc) {
      send_h = std::stoi(argv[++i]);
    } else if (arg == "--fps" && i + 1 < argc) {
      send_fps = std::stoi(argv[++i]);
    } else if (arg == "--bitrate" && i + 1 < argc) {
      send_bitrate = std::stoi(argv[++i]);
    } else if (arg == "--hevc") {
      send_hevc = true;
    } else if (arg == "--camera" && i + 1 < argc) {
      mono_cam = argv[++i];
    } else if (arg == "--stereo-camera" && i + 1 < argc) {
      stereo_cam = argv[++i];
    } else if (arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "未知参数: " << arg << std::endl;
      print_usage(argv[0]);
      return -1;
    }
  }

  if ((listen_mode && send_mode) || (!listen_mode && !send_mode)) {
    std::cerr << "错误：必须且仅能指定一种模式：--listen ADDR 或 --send（配合 "
                 "--server/--port）"
              << std::endl;
    print_usage(argv[0]);
    return -1;
  }

  if (send_mode && (send_server.empty() || send_port <= 0)) {
    std::cerr << "错误：--send 需要 --server IP 与 --port PORT" << std::endl;
    print_usage(argv[0]);
    return -1;
  }

  zed_webcam_install_sigint_handler();
  zed_webcam_set_preview_enabled(preview);
  zed_webcam_set_camera_paths(mono_cam, stereo_cam);

  if (listen_mode) {
    run_listen_mode(listen_addr);
  } else {
    run_send_mode(send_server, send_port, send_w, send_h, send_fps, send_bitrate,
                  send_hevc);
  }

  return 0;
}
