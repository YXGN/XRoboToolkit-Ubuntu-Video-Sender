# XRoboToolkit-Orin-Video-Sender
Video Previewer/Encoder/Sender with ZED default path retained for Ubuntu
deployment (CUDA dependencies removed from default build flags).

![Screenshot](Docs/screenshot.png)
> Sender (Webcam): `./OrinVideoSender --preview --send --server 192.168.1.176 --port 12345`

> Receiver (Video-Viewer): TCP - 192.168.1.176 - 12345 - 1280x720

## Features

- Support Webcam and ZED cameras
- Preview
- H264 Encoding (via GStreamer)
- TCP/UDP sending w/ and w/o ASIO


## Ubuntu deployment (keep ZED default)

- Install dependencies on Ubuntu
```
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libglib2.0-dev \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly gstreamer1.0-libav \
  libopencv-dev libssl-dev libzmq3-dev
```

- Install ZED SDK if you want to run the default ZED target (`main_zed_tcp.cpp`)
  because the default `Makefile` still links against ZED libraries.

- Optional: verify webcam software encoder path (`main_web_gst.cpp`)
```
gst-inspect-1.0 x264enc
```

- Build
```
# Default in Makefile: ZED TCP entry (main_zed_tcp.cpp)
# Webcam fallback with software encoding is available in main_web_gst.cpp.

make

./OrinVideoSender --help

# Listen to coming command from VR, 192.168.1.153 is the Orin IP address
# Add `--preview` to show the video on Orin if necessary 
./OrinVideoSender --listen 192.168.1.153:13579

# send the video stream to both VR via TCP and my own ubuntu via ZMQ
./OrinVideoSender --listen 192.168.1.153:13579 --zmq tcp://*:5555

# Direct send the video stream # 192.168.1.176 is the VR headset IP
# Add `--preview` to show the video on Orin if necessary 
./OrinVideoSender --send --server 192.168.1.176 --port 12345
```

## Notes on platform-specific paths

- ZED-related source files and default build entry are retained.
- CUDA/Jetson-specific linker dependencies are commented out in `Makefile` for
  generic Ubuntu compatibility.
- `main_web_gst.cpp` uses `x264enc` software encoding for non-Jetson webcam use.

## One More Thing 

- For software encoding ffmpeg, please refer to [RobotVisionTest](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/RobotVisionTest).

> Note: Hardware ffmpeg encoding is not availalbe yet.

> Note: Jetson Multimedia API is not in use yet.

- For encoded h264 stream receiver, please refer to [VideoPlayer](https://github.com/XR-Robotics/RobotVision-PC/tree/main/VideoTransferPC/VideoPlayer) [TCP Only].

- For a general video player, please refer to [Video-Viewer](https://github.com/XR-Robotics/XRoboToolkit-Native-Video-Viewer) [TCP/UDP].

- The encoded h264 stream can be also played in [Unity-Client](https://github.com/XR-Robotics/XRoboToolkit-Unity-Client) [TCP Only].
