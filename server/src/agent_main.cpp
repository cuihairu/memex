// R26-1 memex agent：轻量常驻（Linux 起步）。向 memex server 文件面心跳
// 上报本机负载（CPU/内存/磁盘/负载）。注册令牌由群管理员登记服务器时
// 签发（明文只出现一次，agent 只持有并回传；服务端只存 SHA-256 摘要）。
// 只读采样：不开监听口、不落盘、不碰凭据——memex 只做「看」，操作类归
// croupier。心跳间隔默认 30s（服务端 90s 新鲜度窗＝错过两拍仍在线）。
#include <asio.hpp>

#include <nlohmann/json.hpp>

#include <sys/statvfs.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;

// —— CPU：/proc/stat 首行 jiffies 累计，两拍差分出占用率 ——
struct CpuSample {
  std::uint64_t idle{0};
  std::uint64_t total{0};
  bool valid{false};
};

CpuSample read_cpu() {
  std::ifstream f("/proc/stat");
  std::string line;
  if (!std::getline(f, line) || line.rfind("cpu ", 0) != 0) return {};
  std::istringstream in(line.substr(4));
  std::uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
                irq = 0, softirq = 0, steal = 0;
  if (!(in >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
        steal)) {
    return {};
  }
  CpuSample s;
  s.idle = idle + iowait;
  s.total = user + nice + system + idle + iowait + irq + softirq + steal;
  s.valid = true;
  return s;
}

double cpu_percent_between(const CpuSample& a, const CpuSample& b) {
  if (!a.valid || !b.valid || b.total <= a.total) return -1.0;
  const std::uint64_t total = b.total - a.total;
  const std::uint64_t idle = b.idle > a.idle ? b.idle - a.idle : 0;
  return 100.0 * static_cast<double>(total > idle ? total - idle : 0) /
         static_cast<double>(total);
}

// —— 内存：MemTotal／MemAvailable（kB → MB）——
bool read_mem(double& used_mb, double& total_mb) {
  std::ifstream f("/proc/meminfo");
  std::string key, rest;
  std::uint64_t val = 0, total = 0, avail = 0;
  while (f >> key >> val) {
    std::getline(f, rest); // 丢弃行尾单位
    if (key == "MemTotal:") {
      total = val;
    } else if (key == "MemAvailable:") {
      avail = val;
      break;
    }
  }
  if (total == 0) return false;
  total_mb = static_cast<double>(total) / 1024.0;
  used_mb = static_cast<double>(total > avail ? total - avail : 0) / 1024.0;
  return true;
}

// —— 磁盘：statvfs（默认根分区；--path 换挂载点）——
bool read_disk(const std::string& path, double& used_mb, double& total_mb) {
  struct statvfs st;
  if (statvfs(path.c_str(), &st) != 0) return false;
  const double mib = 1024.0 * 1024.0;
  total_mb = static_cast<double>(st.f_blocks) * static_cast<double>(st.f_frsize) /
             mib;
  used_mb = static_cast<double>(st.f_blocks - st.f_bfree) *
                static_cast<double>(st.f_frsize) / mib;
  return true;
}

// —— 系统负载：1 分钟均值（getloadavg）——
double read_load1() {
  double avg[1] = {0.0};
  if (getloadavg(avg, 1) != 1) return 0.0;
  return avg[0];
}

// —— HTTP POST JSON（阻塞短连接；agent 一拍一连，服务端本就逐请求关）——
bool post_json(const std::string& host, const std::string& port,
               const std::string& path, const std::string& body,
               int& status_out) {
  status_out = 0;
  asio::io_context io;
  asio::ip::tcp::socket s(io);
  asio::error_code ec;
  asio::ip::tcp::resolver res(io);
  const auto eps = res.resolve(host, port, ec);
  if (ec) return false;
  asio::connect(s, eps, ec);
  if (ec) return false;
  std::string req = "POST " + path + " HTTP/1.1\r\n"
                    "Host: " + host + "\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
  asio::write(s, asio::buffer(req), ec);
  if (ec) return false;
  std::string raw;
  char buf[4096];
  for (;;) {
    const std::size_t n = s.read_some(asio::buffer(buf), ec);
    if (ec) break;
    raw.append(buf, n);
  }
  if (raw.rfind("HTTP/1.", 0) != 0) return false;
  const auto sp = raw.find(' ');
  status_out = std::atoi(raw.c_str() + sp + 1);
  return status_out >= 200 && status_out < 300;
}

void usage() {
  std::cerr << "用法：memex_agent --server <host>:<files_port> --token <注册令牌>"
               " [--interval 秒] [--path 挂载点] [--once]"
               "\n  --once：发一拍即退（验收/排障用）\n";
}

} // namespace

int main(int argc, char** argv) {
  std::string server, token, disk_path = "/";
  long interval_s = 30;
  bool once = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--server" && i + 1 < argc) {
      server = argv[++i];
    } else if (arg == "--token" && i + 1 < argc) {
      token = argv[++i];
    } else if (arg == "--interval" && i + 1 < argc) {
      interval_s = std::atol(argv[++i]);
    } else if (arg == "--path" && i + 1 < argc) {
      disk_path = argv[++i];
    } else if (arg == "--once") {
      once = true;
    } else {
      usage();
      return 2;
    }
  }
  const auto colon = server.rfind(':');
  if (server.empty() || colon == std::string::npos || colon == 0 ||
      colon + 1 == server.size() || token.empty() || interval_s < 5) {
    usage();
    return 2;
  }
  const std::string host = server.substr(0, colon);
  const std::string port = server.substr(colon + 1);

  // 首拍前先取 250ms 差分窗，让 CPU 占用率第一拍就是真值；之后逐拍
  // 前移 prev，窗口恒等于最近一个心跳间隔
  CpuSample prev = read_cpu();
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  std::cout << "[MEMEX] agent 启动 " << host << ':' << port << " interval="
            << interval_s << "s path=" << disk_path
            << (once ? "（一拍即退）" : "") << std::endl;

  for (;;) {
    const CpuSample now = read_cpu();
    const double cpu = cpu_percent_between(prev, now);
    prev = now;
    double mem_used = 0, mem_total = 0, disk_used = 0, disk_total = 0;
    const bool mem_ok = read_mem(mem_used, mem_total);
    const bool disk_ok = read_disk(disk_path, disk_used, disk_total);
    json body = {{"token", token},
                 {"cpu_percent", cpu},
                 {"mem_used_mb", mem_ok ? mem_used : 0},
                 {"mem_total_mb", mem_ok ? mem_total : 0},
                 {"disk_used_mb", disk_ok ? disk_used : 0},
                 {"disk_total_mb", disk_ok ? disk_total : 0},
                 {"load1", read_load1()}};
    int status = 0;
    if (post_json(host, port, "/files/group-servers/heartbeat", body.dump(),
                  status)) {
      if (once) return 0;
    } else {
      std::cerr << "[MEMEX] agent 心跳失败 status=" << status << "（"
                << interval_s << "s 后重试）" << std::endl;
      if (once) return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(interval_s));
  }
}
