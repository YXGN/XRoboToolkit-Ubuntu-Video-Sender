#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <opencv2/opencv.hpp>
#include <unistd.h>

int main() {
  cv::VideoCapture cap_def("/dev/video12");
  const bool ok_def = cap_def.isOpened();
  std::cout << "open(path,DEFAULT)=" << ok_def << std::endl;
  cap_def.release();

  cv::VideoCapture cap_path("/dev/video12", cv::CAP_V4L2);
  const bool ok_path = cap_path.isOpened();
  std::cout << "open(path,CAP_V4L2)=" << ok_path << std::endl;
  cap_path.release();

  cv::VideoCapture cap_idx_d(12);
  std::cout << "open(index,DEFAULT)=" << cap_idx_d.isOpened() << std::endl;
  cap_idx_d.release();

  cv::VideoCapture cap_idx(12, cv::CAP_V4L2);
  const bool ok_idx = cap_idx.isOpened();
  std::cout << "open(index,CAP_V4L2)=" << ok_idx << std::endl;
  if (cap_idx.isOpened()) {
    cap_idx.set(cv::CAP_PROP_FOURCC,
                cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap_idx.set(cv::CAP_PROP_FRAME_WIDTH, 1856);
    cap_idx.set(cv::CAP_PROP_FRAME_HEIGHT, 800);
    cv::Mat f;
    bool ok = cap_idx.read(f);
    std::cout << "read=" << ok << " rows=" << f.rows << " cols=" << f.cols
              << std::endl;
  }
  cap_idx.release();

  int fd = ::open("/dev/video12", O_RDWR | O_NONBLOCK);
  std::cerr << "open(2) /dev/video12: fd=" << fd;
  if (fd < 0)
    std::cerr << " (" << strerror(errno) << "；EBUSY 多为独占占用)";
  else
    ::close(fd);
  std::cerr << std::endl;

  return 0;
}
