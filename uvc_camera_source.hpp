#ifndef UVC_CAMERA_SOURCE_HPP
#define UVC_CAMERA_SOURCE_HPP

#include <libuvc/libuvc.h>

#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

/* 双目 SBS 候选（按优先级，与 Pico OPEN_CAMERA 2560×720 对齐） */
constexpr int kUvcStereoPrefer1W = 2560;
constexpr int kUvcStereoPrefer1H = 720;
constexpr int kUvcStereoPrefer2W = 3840;
constexpr int kUvcStereoPrefer2H = 1080;
/* 单目首选 */
constexpr int kUvcMonoPreferW = 1920;
constexpr int kUvcMonoPreferH = 1080;

/*
 * libuvc 采集（对齐 teleimager uvc.Capture(uid) + MJPG）。
 * 在 streaming 回调中收 MJPEG，readBgr() 用 OpenCV 解码为 BGR Mat。
 */
class UvcCameraSource {
public:
  UvcCameraSource();
  ~UvcCameraSource();

  UvcCameraSource(const UvcCameraSource &) = delete;
  UvcCameraSource &operator=(const UvcCameraSource &) = delete;

  /*
   * uid: "bus:dev"（与 python uvc.device_list() 一致）；空则不用 uid 筛选
   * serial: 设备序列号；空则不用 serial 筛选
   * 二者都空时打开列表中第一台设备
   */
  bool open(const std::string &uid, const std::string &serial, bool stereo_sbs,
            int fps, std::string &err_detail);

  bool readBgr(cv::Mat &bgr);

  void close();

  bool isOpen() const { return devh_ != nullptr; }

  static void reloadUvcDriver();
  static void listDevices();

private:
  static void frameCallback(uvc_frame_t *frame, void *user_ptr);

  bool tryMjpegMode(int w, int h, int prefer_fps, uvc_stream_ctrl_t &ctrl);
  bool findStreamCtrl(bool stereo_sbs, int fps, uvc_stream_ctrl_t &ctrl,
                      std::string &err_detail);
  bool matchDevice(uvc_device_t *dev, const std::string &uid,
                   const std::string &serial) const;
  static std::string deviceUid(uvc_device_t *dev);

  uvc_context_t *ctx_;
  uvc_device_t *dev_;
  uvc_device_handle_t *devh_;
  uvc_stream_ctrl_t ctrl_;
  bool streaming_;

  std::mutex frame_mutex_;
  std::vector<uint8_t> latest_jpeg_;
  bool has_frame_;
};

#endif
