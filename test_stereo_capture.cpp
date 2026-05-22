/**
 * 与 OrinVideoSender 相同的 libuvc 采集（UvcCameraSource），抓一帧并做 SBS 重叠保存。
 *
 * 用法:
 *   ./test_stereo_capture --uvc-serial 01.00.00 --out capture
 *   ./test_stereo_capture --uvc-uid 1:9 --out capture --fps 60
 *   ./test_stereo_capture --list-devices
 */
#include "uvc_camera_source.hpp"

#include <opencv2/imgcodecs.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

static void saveStereoOverlap(const cv::Mat &bgr, const std::string &prefix,
                              float alpha) {
  const int mid = bgr.cols / 2;
  cv::Mat left = bgr(cv::Rect(0, 0, mid, bgr.rows)).clone();
  cv::Mat right = bgr(cv::Rect(mid, 0, bgr.cols - mid, bgr.rows)).clone();
  if (right.cols != left.cols)
    right = right(cv::Rect(0, 0, left.cols, right.rows)).clone();

  cv::Mat overlap;
  cv::addWeighted(left, alpha, right, 1.f - alpha, 0, overlap);

  cv::imwrite(prefix + "_full.jpg", bgr);
  if (bgr.cols >= 2 * bgr.rows) {
    cv::imwrite(prefix + "_left.jpg", left);
    cv::imwrite(prefix + "_right.jpg", right);
    cv::imwrite(prefix + "_overlap.jpg", overlap);
    std::cout << "stereo SBS " << bgr.cols << "x" << bgr.rows << " -> eye "
              << left.cols << "x" << left.rows << std::endl;
  } else {
    std::cout << "non-SBS " << bgr.cols << "x" << bgr.rows
              << " (only _full.jpg)" << std::endl;
  }
}

static void usage(const char *prog) {
  std::cout
      << "Usage:\n"
      << "  " << prog << " [--list-devices]\n"
      << "  " << prog
      << " [--uvc-uid UID] [--uvc-serial SN] [--fps N] [--alpha A] "
         "[--out PREFIX]\n"
      << "\n与 OrinVideoSender 相同：libuvc + MJPEG + UvcCameraSource。\n";
}

int main(int argc, char **argv) {
  std::string uid;
  std::string serial;
  std::string out_prefix = "capture";
  int fps = 60;
  float alpha = 0.5f;
  bool list_only = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--list-devices" || arg == "-l") {
      list_only = true;
    } else if (arg == "--uvc-uid" && i + 1 < argc) {
      uid = argv[++i];
    } else if (arg == "--uvc-serial" && i + 1 < argc) {
      serial = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      out_prefix = argv[++i];
    } else if (arg == "--fps" && i + 1 < argc) {
      fps = std::atoi(argv[++i]);
    } else if (arg == "--alpha" && i + 1 < argc) {
      alpha = static_cast<float>(std::atof(argv[++i]));
    } else if (arg == "--help" || arg == "-h") {
      usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown argument: " << arg << std::endl;
      usage(argv[0]);
      return 2;
    }
  }

  if (list_only) {
    UvcCameraSource::listDevices();
    return 0;
  }

  if (alpha < 0.f || alpha > 1.f) {
    std::cerr << "error: --alpha must be in [0, 1]" << std::endl;
    return 2;
  }

  UvcCameraSource cam;
  std::string err;
  if (!cam.open(uid, serial, /*stereo_sbs=*/true, fps, err)) {
    std::cerr << "uvc_open failed: " << err << std::endl;
    return 1;
  }

  uint64_t seq = 0;
  if (!cam.waitNewFrame(seq, 5000)) {
    std::cerr << "timeout waiting for first frame" << std::endl;
    cam.close();
    return 1;
  }

  cv::Mat bgr;
  if (!cam.readBgr(bgr) || bgr.empty()) {
    std::cerr << "readBgr failed" << std::endl;
    cam.close();
    return 1;
  }

  std::cout << "frame: " << bgr.cols << "x" << bgr.rows << std::endl;
  saveStereoOverlap(bgr, out_prefix, alpha);
  std::cout << "saved: " << out_prefix << "_{full,left,right,overlap}.jpg"
            << std::endl;
  cam.close();
  return 0;
}
