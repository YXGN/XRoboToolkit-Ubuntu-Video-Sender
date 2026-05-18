#ifndef ZED_WEBCAM_COMMON_HPP
#define ZED_WEBCAM_COMMON_HPP

/*
 * zed_webcam_common.hpp
 *
 * Ubuntu USB 摄像头推流的共享声明：全局配置、线程安全的状态、TCP 发送端，
 * 以及与 listen/send 模式共用的采集→编码→打包发送入口。
 * 采集使用 libuvc + MJPEG（对齐 teleimager uvc.Capture）。
 * 实现见 zed_webcam_common.cpp；listen 专用 TCPServer 在 zed_webcam_listen.cpp。
 */

#include <atomic>
#include <condition_variable>
#include <memory>
#include <utility>
#include <mutex>
#include <string>
#include <thread>

#include "network_helper.hpp"

/* 与 Pico OPEN_CAMERA 载荷对齐的编码/路由参数（send 模式由 CLI 填充同等语义字段） */
struct CameraRequestData {
  int width;
  int height;
  int fps;
  int bitrate;       /* 目标码率，单位 bps；x264enc 使用 kbps，内部会换算 */
  int enableMvHevc;   /* 非 0 则走 x265enc + h265parse，否则 H.264 */
  int renderMode;
  int port;           /* listen 时载荷里的视频端口；send 时与 TCP 目标一致，仅供参考 */
  std::string camera; /* 头显声明的相机类型字符串，Sender 侧一般仅日志 */
  std::string ip;

  CameraRequestData()
      : width(0), height(0), fps(0), bitrate(0), enableMvHevc(0), renderMode(0),
        port(0) {}
};

extern CameraRequestData current_camera_config;

/* 进程级退出标志：SIGINT 与各线程循环均检测 */
extern std::atomic<bool> stop_requested;
/* 推流线程主循环是否继续读摄像头并喂 appsrc */
extern std::atomic<bool> streaming_active;
/* 是否允许向 appsrc 推缓冲（与 streaming_active 协同用于快速停编码） */
extern std::atomic<bool> encoding_enabled;
/* appsink 回调中是否允许向 TCP 发送（连接就绪后为 true） */
extern std::atomic<bool> send_enabled;
extern std::atomic<bool> preview_enabled;

extern std::mutex config_mutex;
extern std::condition_variable streaming_cv;
extern std::mutex streaming_mutex;

/* 视频 TCP：客户端连接 send_to_server:send_to_port，负载为「4 字节大端长度 + 编码 AU」 */
extern std::unique_ptr<TCPClient> sender_ptr;
extern std::string send_to_server;
extern int send_to_port;

/* UVC 采集 CLI（libuvc，非 /dev/video*） */
extern std::string g_cli_uvc_uid;
extern std::string g_cli_uvc_serial;
extern bool g_cli_stereo_mode;

template <typename T, typename... Args>
std::unique_ptr<T> make_unique_helper(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

/* 注册 SIGINT：停止推流、断开 TCP，并通知 listen 侧停 TCPServer */
void zed_webcam_install_sigint_handler();

void zed_webcam_set_preview_enabled(bool v);

void zed_webcam_set_uvc_options(const std::string &uid, const std::string &serial,
                                bool stereo);

/* 按 send_to_server/send_to_port 建立视频 TCP，失败重试至多约 10 次 */
bool initialize_sender();

void startStreamingThread();

void stopStreamingThread();

/* 单实例推流线程主体：连接 → libuvc 采集 → appsrc → GStreamer 编码 → TCP */
void streamingThreadFunction();

/* 定义于 zed_webcam_listen.cpp：停止 Pico 控制口 TCPServer；send 模式未创建服务端则无副作用 */
void zed_webcam_stop_listen_server();

#endif
