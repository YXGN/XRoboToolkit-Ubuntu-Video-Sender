#include "uvc_camera_source.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include <opencv2/imgcodecs.hpp>

namespace {

bool parseUid(const std::string &uid, int &bus, int &addr) {
  return std::sscanf(uid.c_str(), "%d:%d", &bus, &addr) == 2;
}

} // namespace

void UvcCameraSource::reloadUvcDriver() {
  std::cout << "[UVC] Reloading uvcvideo driver..." << std::endl;
  int r1 = std::system("modprobe -r uvcvideo 2>/dev/null");
  (void)r1;
  std::this_thread::sleep_for(std::chrono::seconds(1));
  int r2 = std::system("modprobe uvcvideo 2>/dev/null");
  if (r2 != 0) {
    std::cerr << "[UVC] warn: modprobe uvcvideo returned " << r2
              << " (may need root or driver built-in)" << std::endl;
  } else {
    std::cout << "[UVC] uvcvideo reloaded." << std::endl;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

void UvcCameraSource::listDevices() {
  uvc_context_t *ctx = nullptr;
  if (uvc_init(&ctx, nullptr) != UVC_SUCCESS) {
    std::cerr << "[UVC] uvc_init failed for device list" << std::endl;
    return;
  }
  uvc_device_t **list = nullptr;
  uvc_error_t res = uvc_get_device_list(ctx, &list);
  if (res != UVC_SUCCESS) {
    std::cerr << "[UVC] uvc_get_device_list failed: "
              << uvc_strerror(res) << std::endl;
    uvc_exit(ctx);
    return;
  }
  std::cout << "[UVC] Devices:" << std::endl;
  for (int i = 0; list[i] != nullptr; ++i) {
    uvc_device_descriptor_t *desc = nullptr;
    std::string sn = "?";
    if (uvc_get_device_descriptor(list[i], &desc) == UVC_SUCCESS && desc) {
      if (desc->serialNumber)
        sn = desc->serialNumber;
      std::cout << "  [" << i << "] uid=" << deviceUid(list[i])
                << " vendor=0x" << std::hex << desc->idVendor << std::dec
                << " product=0x" << std::hex << desc->idProduct << std::dec
                << " name=" << (desc->manufacturer ? desc->manufacturer : "")
                << " / " << (desc->product ? desc->product : "")
                << " serial=" << sn << std::endl;
      uvc_free_device_descriptor(desc);
    } else {
      std::cout << "  [" << i << "] uid=" << deviceUid(list[i]) << std::endl;
    }
  }
  uvc_free_device_list(list, 1);
  uvc_exit(ctx);
}

std::string UvcCameraSource::deviceUid(uvc_device_t *dev) {
  std::ostringstream oss;
  oss << static_cast<int>(uvc_get_bus_number(dev)) << ":"
      << static_cast<int>(uvc_get_device_address(dev));
  return oss.str();
}

UvcCameraSource::UvcCameraSource()
    : ctx_(nullptr), dev_(nullptr), devh_(nullptr), streaming_(false),
      has_frame_(false) {
  std::memset(&ctrl_, 0, sizeof(ctrl_));
}

UvcCameraSource::~UvcCameraSource() { close(); }

bool UvcCameraSource::matchDevice(uvc_device_t *dev, const std::string &uid,
                                  const std::string &serial) const {
  if (!uid.empty()) {
    int want_bus = -1, want_addr = -1;
    if (!parseUid(uid, want_bus, want_addr))
      return false;
    if (static_cast<int>(uvc_get_bus_number(dev)) != want_bus ||
        static_cast<int>(uvc_get_device_address(dev)) != want_addr)
      return false;
  }
  if (!serial.empty()) {
    uvc_device_descriptor_t *desc = nullptr;
    if (uvc_get_device_descriptor(dev, &desc) != UVC_SUCCESS || !desc)
      return false;
    bool ok = desc->serialNumber && (serial == desc->serialNumber);
    uvc_free_device_descriptor(desc);
    if (!ok)
      return false;
  }
  return true;
}

bool UvcCameraSource::tryMjpegMode(int w, int h, int prefer_fps,
                                   uvc_stream_ctrl_t &ctrl) {
  const int fps_candidates[] = {prefer_fps, 30, 60, 25, 20, 15, 10, 5, 2};
  for (int try_fps : fps_candidates) {
    if (try_fps <= 0)
      continue;
    uvc_error_t res = uvc_get_stream_ctrl_format_size(
        devh_, &ctrl, UVC_FRAME_FORMAT_MJPEG, w, h,
        static_cast<uint32_t>(try_fps));
    if (res == UVC_SUCCESS) {
      std::cout << "[UVC] MJPEG mode " << w << "x" << h << "@" << try_fps
                << std::endl;
      return true;
    }
  }
  return false;
}

bool UvcCameraSource::findStreamCtrl(bool stereo_sbs, int fps,
                                     uvc_stream_ctrl_t &ctrl,
                                     std::string &err_detail) {
  const int prefer_fps = (fps > 0) ? fps : 30;

  if (stereo_sbs) {
    if (tryMjpegMode(kUvcStereoPrefer1W, kUvcStereoPrefer1H, prefer_fps, ctrl))
      return true;
    if (tryMjpegMode(kUvcStereoPrefer2W, kUvcStereoPrefer2H, prefer_fps, ctrl))
      return true;
    err_detail = "no stereo MJPEG (tried 3840x1080, 2560x720)";
    return false;
  }

  if (tryMjpegMode(kUvcMonoPreferW, kUvcMonoPreferH, prefer_fps, ctrl))
    return true;

  const int fallbacks[][2] = {{2560, 720}, {1280, 720}, {1280, 480},
                              {640, 480}};
  for (const auto &fb : fallbacks) {
    if (tryMjpegMode(fb[0], fb[1], prefer_fps, ctrl))
      return true;
  }

  err_detail = "no mono MJPEG (tried 1920x1080 and fallbacks)";
  return false;
}

bool UvcCameraSource::open(const std::string &uid, const std::string &serial,
                           bool stereo_sbs, int fps, std::string &err_detail) {
  close();
  err_detail.clear();

  if (fps <= 0)
    fps = 30;

  reloadUvcDriver();
  listDevices();

  uvc_error_t res = uvc_init(&ctx_, nullptr);
  if (res != UVC_SUCCESS) {
    err_detail = std::string("uvc_init: ") + uvc_strerror(res);
    return false;
  }

  uvc_device_t **list = nullptr;
  res = uvc_get_device_list(ctx_, &list);
  if (res != UVC_SUCCESS) {
    err_detail = std::string("uvc_get_device_list: ") + uvc_strerror(res);
    uvc_exit(ctx_);
    ctx_ = nullptr;
    return false;
  }

  dev_ = nullptr;
  if (!uid.empty() || !serial.empty()) {
    for (int i = 0; list[i] != nullptr; ++i) {
      if (matchDevice(list[i], uid, serial)) {
        dev_ = list[i];
        break;
      }
    }
    if (!dev_) {
      err_detail = "no device matching uid/serial";
      uvc_free_device_list(list, 1);
      uvc_exit(ctx_);
      ctx_ = nullptr;
      return false;
    }
  } else {
    if (!list[0]) {
      err_detail = "no UVC devices found";
      uvc_free_device_list(list, 1);
      uvc_exit(ctx_);
      ctx_ = nullptr;
      return false;
    }
    dev_ = list[0];
    std::cout << "[UVC] auto-selected uid=" << deviceUid(dev_) << std::endl;
  }

  uvc_ref_device(dev_);
  res = uvc_open(dev_, &devh_);
  uvc_free_device_list(list, 1);
  if (res != UVC_SUCCESS) {
    err_detail = std::string("uvc_open: ") + uvc_strerror(res);
    uvc_unref_device(dev_);
    dev_ = nullptr;
    uvc_exit(ctx_);
    ctx_ = nullptr;
    devh_ = nullptr;
    return false;
  }

  if (!findStreamCtrl(stereo_sbs, fps, ctrl_, err_detail)) {
    uvc_close(devh_);
    devh_ = nullptr;
    uvc_unref_device(dev_);
    dev_ = nullptr;
    uvc_exit(ctx_);
    ctx_ = nullptr;
    return false;
  }

  res = uvc_start_streaming(devh_, &ctrl_, &UvcCameraSource::frameCallback,
                            this, 0);
  if (res != UVC_SUCCESS) {
    err_detail = std::string("uvc_start_streaming: ") + uvc_strerror(res);
    uvc_close(devh_);
    devh_ = nullptr;
    uvc_unref_device(dev_);
    dev_ = nullptr;
    uvc_exit(ctx_);
    ctx_ = nullptr;
    return false;
  }
  streaming_ = true;

  /* 等待首帧 */
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard<std::mutex> lock(frame_mutex_);
      if (has_frame_)
        break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_frame_) {
      err_detail = "timeout waiting for first MJPEG frame";
      close();
      return false;
    }
  }

  std::cout << "[UVC] streaming started uid=" << deviceUid(dev_) << std::endl;
  return true;
}

void UvcCameraSource::frameCallback(uvc_frame_t *frame, void *user_ptr) {
  auto *self = static_cast<UvcCameraSource *>(user_ptr);
  if (!frame || !frame->data || frame->data_bytes == 0)
    return;
  if (frame->frame_format != UVC_FRAME_FORMAT_MJPEG)
    return;

  std::lock_guard<std::mutex> lock(self->frame_mutex_);
  self->latest_jpeg_.assign(static_cast<uint8_t *>(frame->data),
                            static_cast<uint8_t *>(frame->data) +
                                frame->data_bytes);
  self->has_frame_ = true;
}

bool UvcCameraSource::readBgr(cv::Mat &bgr) {
  std::vector<uint8_t> jpeg_copy;
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_frame_ || latest_jpeg_.empty())
      return false;
    jpeg_copy = latest_jpeg_;
  }

  cv::Mat buf(1, static_cast<int>(jpeg_copy.size()), CV_8UC1, jpeg_copy.data());
  bgr = cv::imdecode(buf, cv::IMREAD_COLOR);
  return !bgr.empty();
}

void UvcCameraSource::close() {
  if (devh_ && streaming_) {
    uvc_stop_streaming(devh_);
    streaming_ = false;
  }
  if (devh_) {
    uvc_close(devh_);
    devh_ = nullptr;
  }
  if (dev_) {
    uvc_unref_device(dev_);
    dev_ = nullptr;
  }
  if (ctx_) {
    uvc_exit(ctx_);
    ctx_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    latest_jpeg_.clear();
    has_frame_ = false;
  }
}
