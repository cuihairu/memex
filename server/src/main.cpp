// Memex 协作服务端骨架（T0.3）：
// 监听 TCP 端口，接受长连接，回显心跳；SIGINT／SIGTERM 优雅退出。
// 归档、路由等模块按 todo 阶段 2 逐步接入。
#include <asio.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <memex/protocol/messages.hpp>

namespace {

using asio::ip::tcp;

// 服务端默认监听端口。评审报告只固定了直连态端口段（UDP 2425–2436／TCP 2426–2437），
// 服务端监听端口未钉死，此默认值待与网络管理侧确认后写入部署文档。
constexpr std::uint16_t kDefaultPort = 24360;

class Session : public std::enable_shared_from_this<Session> {
public:
  explicit Session(tcp::socket socket) : socket_(std::move(socket)) {}

  void start() {
    log("接入");
    do_read();
  }

private:
  void do_read() {
    auto self = shared_from_this();
    socket_.async_read_some(
        asio::buffer(read_buf_),
        [this, self](std::error_code ec, std::size_t n) {
          if (ec) {
            log(std::string{"断开："} + ec.message());
            return;
          }
          handle_bytes(n);
          if (!closed_) do_read();
        });
  }

  void handle_bytes(std::size_t n) {
    std::vector<std::string> frames;
    const auto st = decoder_.feed(std::string_view(read_buf_.data(), n), frames);
    if (st == memex::protocol::DecodeStatus::kZeroLength ||
        st == memex::protocol::DecodeStatus::kTooLarge) {
      log(std::string{"非法帧："} + memex::protocol::decode_status_name(st));
      close();
      return;
    }
    for (const auto& f : frames) {
      try {
        const auto msg = memex::protocol::Message::decode_payload(f);
        log(std::string{"收到 "} + memex::protocol::msg_type_name(msg.type));
        if (msg.type == memex::protocol::MsgType::kPing) {
          memex::protocol::Message pong;
          pong.type = memex::protocol::MsgType::kPong;
          pong.seq = msg.seq;
          pong.from = "server";
          pong.to = msg.from;
          pong.ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
          pong.body = nlohmann::json::object();
          write_queue_.push_back(pong.encode());
        }
      } catch (const memex::protocol::ProtocolError& e) {
        log(std::string{"协议错误："} + e.what());
      }
    }
    if (!write_queue_.empty()) do_write();
  }

  void do_write() {
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(write_queue_.front()),
                      [this, self](std::error_code ec, std::size_t) {
                        if (ec) {
                          close();
                          return;
                        }
                        write_queue_.pop_front();
                        if (!write_queue_.empty()) do_write();
                      });
  }

  void close() {
    closed_ = true;
    std::error_code ignore;
    socket_.shutdown(tcp::socket::shutdown_both, ignore);
    socket_.close(ignore);
  }

  void log(const std::string& what) const {
    std::cout << "[MEMEX][session " << remote_ << "] " << what << std::endl;
  }

  tcp::socket socket_;
  std::string remote_{
      [&] {
        try {
          return socket_.remote_endpoint().address().to_string();
        } catch (...) {
          return std::string{"?"};
        }
      }()};
  memex::protocol::FrameDecoder decoder_;
  std::array<char, 65536> read_buf_{};
  std::deque<std::string> write_queue_;
  bool closed_{false};
};

class Server {
public:
  Server(asio::io_context& io, std::uint16_t port)
      : acceptor_(io, tcp::endpoint(tcp::v4(), port)) {}

  void run() {
    std::cout << "[MEMEX] MemexServer 监听 0.0.0.0:" << acceptor_.local_endpoint().port()
              << std::endl;
    do_accept();
  }

private:
  void do_accept() {
    acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
      if (!ec) {
        std::make_shared<Session>(std::move(socket))->start();
      }
      do_accept();
    });
  }

  tcp::acceptor acceptor_;
};

int self_test() {
  asio::io_context io;
  tcp::acceptor a(io, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
  std::cout << "self-test ok: listen 127.0.0.1:" << a.local_endpoint().port()
            << ", protocol " << memex::protocol::msg_type_name(
                                   memex::protocol::MsgType::kHello)
            << std::endl;
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::uint16_t port = kDefaultPort;

  if (argc > 1 && std::string_view(argv[1]) == "--self-test") {
    return self_test();
  }
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      if (port == 0) {
        std::cerr << "无效端口\n";
        return 2;
      }
    }
  }

  asio::io_context io;
  asio::signal_set signals(io, SIGINT, SIGTERM);
  signals.async_wait([&](std::error_code, int sig) {
    std::cout << "[MEMEX] 收到信号 " << sig << "，退出" << std::endl;
    io.stop();
  });

  try {
    Server server(io, port);
    server.run();
    io.run();
  } catch (const std::exception& e) {
    std::cerr << "服务端异常退出：" << e.what() << std::endl;
    return 1;
  }
  return 0;
}
