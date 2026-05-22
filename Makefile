###############################################################################
#
# Standalone Makefile for OrinVideoSender
# Self-contained build configuration
# By liuchuan.yu@bytedance.com
#
###############################################################################

# Compiler settings
CXX = g++
APP := OrinVideoSender

###############################################################################
# 默认入口（Ubuntu）：USB 摄像头 --listen（Pico）或 --send（直连 TCP）
# main_zed_webcam.cpp + zed_webcam_{common,listen,send}.cpp
###############################################################################

SRCS := \
	main_zed_webcam.cpp \
	uvc_camera_source.cpp \
	zed_webcam_common.cpp \
	zed_webcam_listen.cpp \
	zed_webcam_send.cpp

###############################################################################
# 备选入口（按需取消注释其一，并注释掉上方默认 SRCS）
###############################################################################

# USB 摄像头直推 TCP（无 Pico 控制通道）
# SRCS := \
# 	main_web_gst.cpp

# Jetson + ZED SDK 真机路径（需 /usr/local/zed、CUDA 等）
# SRCS := \
# 	main_zed_tcp.cpp

#SRCS := \
#	main_zed_tcp_zmq.cpp

# # TCP with asio -- pass
# SRCS := \
# 	main_zed_asio.cpp

# # UDP w/ asio -- pass
# SRCS:= \
# 	main_zed_asio_udp.cpp

# # [NOT WORKING] Zero Copy - depends on jetson multimedia api
# SRCS := \
# 	main_zed_zero_copy.cpp
###############################################################################

OBJS := $(SRCS:.cpp=.o)

# Include paths（无 ZED/CUDA；保留 OpenCV 供 USB 采集）
CPPFLAGS := -std=c++11 \
	-I./asio-1.30.2/include \
	-I/usr/include/opencv4 \
	$(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 glib-2.0 2>/dev/null || echo "") \
	$(shell pkg-config --cflags libzmq libuvc 2>/dev/null || echo "") \
	$(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo "")

# Compiler flags
CXXFLAGS := -Wall -Wextra -O2 -g

# FFmpeg（与历史 Makefile 一致；当前默认入口未直接调用 FFmpeg API，可保留链接）
CXXFLAGS += $(shell pkg-config --cflags libavcodec libavformat libavutil libswscale libavdevice 2>/dev/null || echo "")

# Library paths and libraries（Jetson/CUDA 路径保持注释，便于真机分支恢复）
LDFLAGS := \
	# -L/usr/local/cuda/lib64
	# -L/usr/lib/aarch64-linux-gnu

LDFLAGS += $(shell pkg-config --libs libavcodec libavformat libavutil libswscale libavdevice 2>/dev/null || echo "-lavcodec -lavformat -lavutil -lswscale -lavdevice")

# 以下为默认 Ubuntu 链接；Jetson 上恢复 CUDA 时可取消注释并加入 -lcuda -lcudart
LDFLAGS += -lopencv_core -lopencv_imgproc -lopencv_videoio -lopencv_imgcodecs \
	-lssl -lcrypto \
	-lpthread \
	-lstdc++

LDFLAGS += $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 glib-2.0 2>/dev/null || echo "-lgstreamer-1.0 -lgstapp-1.0 -lglib-2.0")

LDFLAGS += $(shell pkg-config --libs libzmq 2>/dev/null || echo "-lzmq")

LDFLAGS += $(shell pkg-config --libs libuvc libusb-1.0 2>/dev/null || echo "-luvc -lusb-1.0")

# 与 OrinVideoSender 相同 libuvc 路径的抓帧测试（不链 GStreamer/ZMQ）
TEST_STEREO := test_stereo_capture
TEST_STEREO_OBJS := test_stereo_capture.o uvc_camera_source.o
TEST_LDFLAGS := -lopencv_core -lopencv_imgproc -lopencv_imgcodecs -lpthread \
	-lstdc++ \
	$(shell pkg-config --libs libuvc libusb-1.0 2>/dev/null || echo "-luvc -lusb-1.0")

all: $(APP)

test-stereo: $(TEST_STEREO)

$(TEST_STEREO): $(TEST_STEREO_OBJS)
	@echo "Linking: $@"
	$(CXX) -o $@ $(TEST_STEREO_OBJS) $(TEST_LDFLAGS)

test_stereo_capture.o: test_stereo_capture.cpp uvc_camera_source.hpp
	@echo "Compiling: $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

debug: CXXFLAGS += -DDEBUG -g3 -O0
debug: $(APP)

%.o: %.cpp
	@echo "Compiling: $<"
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(APP): $(OBJS)
	@echo "Linking: $@"
	$(CXX) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -rf $(APP) $(OBJS) $(TEST_STEREO) $(TEST_STEREO_OBJS)

install: $(APP)
	@echo "Installing $(APP)..."
	install -D $(APP) /usr/local/bin/$(APP)

.PHONY: all debug clean install test-stereo
