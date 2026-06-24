#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include <opencv2/opencv.hpp>

namespace {

struct Args {
  std::string device = "/dev/video0";
  std::string backend = "v4l2";
  int width = 640;
  int height = 480;
  int fps = 30;
  std::string fourcc = "MJPG";
  int buffersize = 1;
  int count = 300;
  int report_every = 60;
};

void printUsage(const char *argv0) {
  std::cout
      << "Usage: " << argv0
      << " [--device /dev/video0] [--backend v4l2|default|gstreamer]\n"
      << "       [--width 640] [--height 480] [--fps 30] [--fourcc MJPG]\n"
      << "       [--buffersize 1] [--count 300] [--report-every 60]\n";
}

bool parseArgs(int argc, char **argv, Args &args) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto needValue = [&](const char *name) -> const char * {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << std::endl;
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--device") {
      const char *v = needValue("--device");
      if (!v) return false;
      args.device = v;
    } else if (arg == "--backend") {
      const char *v = needValue("--backend");
      if (!v) return false;
      args.backend = v;
    } else if (arg == "--width") {
      const char *v = needValue("--width");
      if (!v) return false;
      args.width = std::stoi(v);
    } else if (arg == "--height") {
      const char *v = needValue("--height");
      if (!v) return false;
      args.height = std::stoi(v);
    } else if (arg == "--fps") {
      const char *v = needValue("--fps");
      if (!v) return false;
      args.fps = std::stoi(v);
    } else if (arg == "--fourcc") {
      const char *v = needValue("--fourcc");
      if (!v) return false;
      args.fourcc = v;
    } else if (arg == "--buffersize") {
      const char *v = needValue("--buffersize");
      if (!v) return false;
      args.buffersize = std::stoi(v);
    } else if (arg == "--count") {
      const char *v = needValue("--count");
      if (!v) return false;
      args.count = std::stoi(v);
    } else if (arg == "--report-every") {
      const char *v = needValue("--report-every");
      if (!v) return false;
      args.report_every = std::stoi(v);
    } else if (arg == "--help") {
      printUsage(argv[0]);
      std::exit(0);
    } else {
      std::cerr << "unknown arg: " << arg << std::endl;
      return false;
    }
  }
  return true;
}

std::string fourccToStr(double value) {
  int code = static_cast<int>(value);
  char s[5] = {
      static_cast<char>(code & 0xFF),
      static_cast<char>((code >> 8) & 0xFF),
      static_cast<char>((code >> 16) & 0xFF),
      static_cast<char>((code >> 24) & 0xFF),
      '\0',
  };
  return std::string(s);
}

int backendFlag(const std::string &backend) {
  if (backend == "v4l2") return cv::CAP_V4L2;
  if (backend == "gstreamer") return cv::CAP_GSTREAMER;
  return cv::CAP_ANY;
}

cv::VideoCapture openCapture(const Args &args, std::string &open_desc) {
  if (args.backend == "gstreamer") {
    std::string fmt = args.fourcc;
    for (char &c : fmt) c = static_cast<char>(std::toupper(c));

    std::string src_caps;
    std::string decode;
    if (fmt == "MJPG") {
      src_caps = "image/jpeg,width=" + std::to_string(args.width) +
                 ",height=" + std::to_string(args.height) +
                 ",framerate=" + std::to_string(args.fps) + "/1";
      decode = "jpegdec ! ";
    } else if (fmt == "YUYV") {
      src_caps = "video/x-raw,format=YUY2,width=" + std::to_string(args.width) +
                 ",height=" + std::to_string(args.height) +
                 ",framerate=" + std::to_string(args.fps) + "/1";
    } else {
      throw std::runtime_error("gstreamer backend only supports MJPG or YUYV");
    }

    std::ostringstream oss;
    oss << "v4l2src device=" << args.device
        << " io-mode=2 do-timestamp=true ! "
        << src_caps << " ! "
        << decode
        << "videoconvert ! video/x-raw,format=BGR ! "
        << "appsink drop=true max-buffers=1 sync=false";
    open_desc = oss.str();
    return cv::VideoCapture(open_desc, cv::CAP_GSTREAMER);
  }

  open_desc = args.device;
  return cv::VideoCapture(args.device, backendFlag(args.backend));
}

}  // namespace

int main(int argc, char **argv) {
  Args args;
  if (!parseArgs(argc, argv, args)) {
    printUsage(argv[0]);
    return 1;
  }

  try {
    std::string open_desc;
    cv::VideoCapture cap = openCapture(args, open_desc);
    if (!cap.isOpened()) {
      std::cerr << "[probe_cpp] failed to open " << open_desc
                << " backend=" << args.backend << std::endl;
      return 2;
    }

    std::cout << "[probe_cpp] opened device=" << args.device
              << " backend=" << args.backend
              << " request=" << args.width << "x" << args.height << "@"
              << args.fps << " fourcc=" << args.fourcc
              << " buffersize=" << args.buffersize << std::endl;
    if (args.backend == "gstreamer") {
      std::cout << "[probe_cpp] gstreamer pipeline: " << open_desc << std::endl;
    }

    if (args.backend != "gstreamer") {
      if (args.fourcc.size() != 4) {
        std::cerr << "--fourcc must be exactly 4 chars" << std::endl;
        return 3;
      }
      cap.set(cv::CAP_PROP_FOURCC,
              cv::VideoWriter::fourcc(args.fourcc[0], args.fourcc[1],
                                      args.fourcc[2], args.fourcc[3]));
      cap.set(cv::CAP_PROP_FRAME_WIDTH, args.width);
      cap.set(cv::CAP_PROP_FRAME_HEIGHT, args.height);
      cap.set(cv::CAP_PROP_FPS, args.fps);
      cap.set(cv::CAP_PROP_BUFFERSIZE, args.buffersize);
    }

    std::cout << "[probe_cpp] actual size="
              << static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH)) << "x"
              << static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT))
              << " fps=" << cap.get(cv::CAP_PROP_FPS)
              << " fourcc=" << fourccToStr(cap.get(cv::CAP_PROP_FOURCC))
              << std::endl;

    cv::Mat frame;
    int total_count = 0;
    int report_count = 0;
    auto total_start = std::chrono::steady_clock::now();
    auto report_start = total_start;

    while (total_count < args.count) {
      if (!cap.read(frame) || frame.empty()) {
        std::cerr << "[probe_cpp] read failed at frame=" << total_count
                  << std::endl;
        break;
      }
      ++total_count;
      ++report_count;

      if (report_count >= args.report_every) {
        auto now = std::chrono::steady_clock::now();
        double elapsed =
            std::chrono::duration_cast<std::chrono::duration<double>>(now -
                                                                      report_start)
                .count();
        double fps = elapsed > 0.0 ? static_cast<double>(report_count) / elapsed
                                   : 0.0;
        std::cout << "[probe_cpp] frames=" << total_count
                  << " interval_fps=" << fps
                  << " shape=" << frame.cols << "x" << frame.rows << "x"
                  << frame.channels() << std::endl;
        report_count = 0;
        report_start = now;
      }
    }

    auto total_end = std::chrono::steady_clock::now();
    double total_elapsed =
        std::chrono::duration_cast<std::chrono::duration<double>>(total_end -
                                                                  total_start)
            .count();
    double total_fps =
        total_elapsed > 0.0 ? static_cast<double>(total_count) / total_elapsed
                            : 0.0;
    std::cout << "[probe_cpp] summary frames=" << total_count
              << " elapsed=" << total_elapsed
              << " fps=" << total_fps << std::endl;
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "[probe_cpp] exception: " << e.what() << std::endl;
    return 4;
  }
}
