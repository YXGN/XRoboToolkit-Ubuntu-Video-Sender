#ifndef NETWORK_ASIO_HPP
#define NETWORK_ASIO_HPP

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

// kUdpMaxPayload: 1500(ETH) - 20(IP) - 8(UDP) = 1472 bytes max per datagram
// Fragment header: 1(marker) + 1(is_last) + 4(total_size) + 4(offset) = 10 bytes
// So payload per fragment = 1472 - 10 = 1462 bytes
const int kUdpMaxPayload = 1462;

namespace asio_net {

class UDPException : public std::exception {
private:
  std::string message;

public:
  UDPException(const std::string &msg) : message(msg) {}
  const char *what() const noexcept override { return message.c_str(); }
};

using TCPException = UDPException;

typedef UDPException TCPException;

class TCPClient {
private:
  asio::io_context io_context;
  asio::ip::tcp::socket socket;
  std::string server_ip;
  int server_port;
  std::atomic<bool> connected;
  std::thread io_thread;

public:
  TCPClient(const std::string &ip, int port)
      : socket(io_context), server_ip(ip), server_port(port), connected(false) {
  }

  ~TCPClient() { disconnect(); }

  bool connect() {
    if (connected)
      return true;

    try {
      asio::ip::tcp::resolver resolver(io_context);
      auto endpoints = resolver.resolve(server_ip, std::to_string(server_port));

      asio::connect(socket, endpoints);
      connected = true;

      asio::ip::tcp::no_delay option(true);
      socket.set_option(option);

      asio::socket_base::keep_alive option2(false);
      socket.set_option(option2);

      io_thread = std::thread([this]() { io_context.run(); });

      std::cout << "Connected to server " << server_ip << ":" << server_port
                << " (TCP_NODELAY enabled)" << std::endl;
      return true;
    } catch (const std::exception &e) {
      throw UDPException("Connection failed: " + std::string(e.what()));
    }
  }

  void disconnect() {
    connected = false;

    if (socket.is_open()) {
      std::error_code ec;
      socket.close(ec);
    }

    io_context.stop();

    if (io_thread.joinable()) {
      io_thread.join();
    }

    io_context.restart();
  }

  bool isConnected() const { return connected && socket.is_open(); }

  void sendData(const char *data, uint32_t size) {
    if (!connected || !socket.is_open()) {
      throw UDPException("Not connected to server");
    }

    if (!data || size == 0) {
      throw UDPException("Invalid data or size");
    }

    try {
      asio::write(socket, asio::buffer(data, size));
    } catch (const std::exception &e) {
      connected = false;
      throw UDPException("Send failed: " + std::string(e.what()));
    }
  }

  void sendData(const std::vector<uint8_t> &data) {
    sendData(reinterpret_cast<const char *>(data.data()),
             static_cast<uint32_t>(data.size()));
  }
};

class TCPServer {
private:
  asio::io_context io_context;
  asio::ip::tcp::acceptor acceptor;
  std::shared_ptr<asio::ip::tcp::socket> client_socket;
  int server_port;
  std::atomic<bool> client_connected;
  std::atomic<bool> server_running;
  std::thread server_thread;
  std::promise<void> exit_signal;
  std::future<void> exit_future;

  std::function<void(const std::string &)> data_callback;

public:
  TCPServer(const std::string &address)
      : acceptor(io_context),
        server_port(0),
        client_connected(false),
        server_running(false),
        exit_future(exit_signal.get_future()) {
    std::string host = address;
    std::string port_str;

    size_t colon_pos = address.find(':');
    if (colon_pos != std::string::npos) {
      host = address.substr(0, colon_pos);
      port_str = address.substr(colon_pos + 1);
      server_port = std::stoi(port_str);
    } else {
      host = "0.0.0.0";
      port_str = address;
      server_port = std::stoi(port_str);
    }

    asio::ip::tcp::endpoint endpoint(asio::ip::address::from_string(host), server_port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
  }

  TCPServer(int port)
      : acceptor(io_context),
        server_port(port),
        client_connected(false),
        server_running(false),
        exit_future(exit_signal.get_future()) {
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), server_port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
  }

  ~TCPServer() { stop(); }

  void setDataCallback(std::function<void(const std::string &)> callback) {
    data_callback = callback;
  }

  void acceptNext() {
    acceptor.async_accept(
        [this](std::error_code ec, asio::ip::tcp::socket socket) {
          if (!ec) {
            client_socket = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            client_connected = true;
            std::cout << "Client connected" << std::endl;

            readLoop();
          }
        });
  }

  void start() {
    if (server_running)
      return;

    server_running = true;
    server_thread = std::thread([this]() {
      acceptNext();
      io_context.run();
    });
  }

  void stop() {
    server_running = false;
    client_connected = false;

    if (client_socket && client_socket->is_open()) {
      std::error_code ec;
      client_socket->close(ec);
    }

    std::error_code ec;
    acceptor.close(ec);

    io_context.stop();

    if (server_thread.joinable()) {
      server_thread.join();
    }

    io_context.restart();
  }

  bool isClientConnected() const { return client_connected; }

  void sendData(const char *data, uint32_t size) {
    if (!client_connected || !client_socket || !client_socket->is_open()) {
      throw UDPException("No client connected");
    }

    try {
      asio::write(*client_socket, asio::buffer(data, size));
    } catch (const std::exception &e) {
      client_connected = false;
      throw UDPException("Send failed: " + std::string(e.what()));
    }
  }

  void sendData(const std::vector<uint8_t> &data) {
    sendData(reinterpret_cast<const char *>(data.data()),
             static_cast<uint32_t>(data.size()));
  }

  void disconnectClient() {
    if (client_socket && client_socket->is_open()) {
      std::error_code ec;
      client_socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
      client_socket->close(ec);
    }
    client_connected = false;
  }

private:
  void readLoop() {
    if (!client_socket || !client_socket->is_open()) {
      return;
    }

    auto buffer = std::make_shared<std::vector<uint8_t>>(8192);

    client_socket->async_read_some(
        asio::buffer(*buffer),
        [this, buffer](std::error_code ec, std::size_t bytes_transferred) {
          if (!ec) {
            std::string data(buffer->begin(), buffer->begin() + bytes_transferred);
            if (data_callback) {
              data_callback(data);
            }
            readLoop();
          } else {
            client_connected = false;
            std::cout << "Client disconnected" << std::endl;
          }
        });
  }
};

class UDPClient {
private:
  asio::io_context io_context;
  asio::ip::udp::socket socket;
  asio::ip::udp::endpoint receiver_endpoint;
  std::thread io_thread;
  std::atomic<bool> running;
  std::atomic<bool> connected;

public:
  UDPClient(const std::string &ip, int port)
      : socket(io_context),
        receiver_endpoint(asio::ip::make_address(ip), static_cast<unsigned short>(port)),
        running(false),
        connected(false) {
    socket.open(asio::ip::udp::v4());
    connected = true;
  }

  ~UDPClient() { stop(); }

  void connect() {
    if (!socket.is_open()) {
      socket.open(asio::ip::udp::v4());
    }
    connected = true;
  }

  void disconnect() { stop(); }

  bool isConnected() const { return connected; }

  void sendData(const char *data, uint32_t size) {
    if (!socket.is_open()) {
      throw UDPException("Socket not open");
    }

    try {
      socket.send_to(asio::buffer(data, size), receiver_endpoint);
    } catch (const std::exception &e) {
      throw UDPException(std::string("Send failed: ") + e.what());
    }
  }

  void sendData(const std::vector<uint8_t> &data) {
    sendData(reinterpret_cast<const char *>(data.data()),
             static_cast<uint32_t>(data.size()));
  }

  void sendDataAsync(const char *data, uint32_t size) {
    sendData(data, size);
  }

  void sendDataAsync(const std::vector<uint8_t> &data) {
    sendData(data);
  }

  void startReceive(
      std::function<void(const char *, uint32_t, const asio::ip::udp::endpoint &)> callback) {
    running = true;

    std::vector<uint8_t> recv_buffer(65536);
    socket.async_receive_from(
        asio::buffer(recv_buffer),
        receiver_endpoint,
        [this, &recv_buffer, callback](std::error_code ec, std::size_t bytes_recvd) {
          if (!ec) {
            callback(reinterpret_cast<const char *>(recv_buffer.data()),
                    static_cast<uint32_t>(bytes_recvd), receiver_endpoint);
            if (running) {
              startReceive(callback);
            }
          }
        });

    io_thread = std::thread([this]() { io_context.run(); });
  }

  void stop() {
    running = false;
    connected = false;
    if (socket.is_open()) {
      socket.close();
    }
    io_context.stop();
    if (io_thread.joinable()) {
      io_thread.join();
    }
  }
};

} // namespace asio_net

#endif // NETWORK_ASIO_HPP
