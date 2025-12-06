#include "run_modes.h"
#include "fix_demo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace demo = fix_demo;

namespace {

std::string allocatorLabel(AllocatorPolicy policy) {
  return (policy == AllocatorPolicy::Arena) ? "arena" : "heap";
}

std::string processWithArena(const std::string& fixMessage,
                             demo::ArenaAllocator& arena,
                             std::ostream* outputStream = nullptr,
                             std::string_view arrivalTime = {}) {
  demo::ArenaScope scope(arena);
  auto order = demo::createOrderOnArena(arena, fixMessage);
  if (!arrivalTime.empty()) {
    order->arrivalTime = std::string(arrivalTime);
  }
  const std::string json = demo::toJson(*order);
  if (outputStream) {
    (*outputStream) << json << '\n';
  }
  return json;
}

std::string processWithNew(const std::string& fixMessage,
                           std::ostream* outputStream = nullptr,
                           std::string_view arrivalTime = {}) {
  auto order = demo::createOrderOnHeap(fixMessage);
  if (!arrivalTime.empty()) {
    order->arrivalTime = std::string(arrivalTime);
  }
  const std::string json = demo::toJson(*order);
  if (outputStream) {
    (*outputStream) << json << '\n';
  }
  return json;
}

std::vector<std::string> loadFixMessages(const std::filesystem::path& path) {
  std::vector<std::string> messages;
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("unable to open fix message file: " + path.string());
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      messages.push_back(line);
    }
  }
  return messages;
}

template <typename Handler>
void benchmark(const std::vector<std::string>& messages,
               Handler handler,
               const std::string& label) {
  auto start = std::chrono::steady_clock::now();
  for (const auto& message : messages) {
    handler(message);
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  std::cout << label << " processed " << messages.size() << " messages in "
            << ms << " ms\n";
}

struct SessionEndpoint {
  std::string host;
  uint16_t port = 0;
};

struct ServerConfig {
  SessionEndpoint arena;
  SessionEndpoint heap;
};

std::string formatTimestamp(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const auto t = system_clock::to_time_t(tp);
  std::tm tm{};
#ifdef _WIN32
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  char buffer[64];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &tm) == 0) {
    return {};
  }
  const auto micros = duration_cast<microseconds>(tp.time_since_epoch()) % seconds(1);
  char withFraction[80];
  std::snprintf(withFraction, sizeof(withFraction), "%s.%06lldZ", buffer,
                static_cast<long long>(micros.count()));
  return std::string(withFraction);
}

std::string formatLocalTimeMs(std::chrono::system_clock::time_point tp) {
  using namespace std::chrono;
  const auto t = system_clock::to_time_t(tp);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm) == 0) {
    return {};
  }
  const auto millis = duration_cast<milliseconds>(tp.time_since_epoch()) % seconds(1);
  char withFraction[48];
  std::snprintf(withFraction, sizeof(withFraction), "%s.%03lld", buffer,
                static_cast<long long>(millis.count()));
  return std::string(withFraction);
}

static std::string trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(begin, end - begin + 1));
}

static ServerConfig loadServerConfig(const std::filesystem::path& path) {
  ServerConfig config;
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("unable to open server config: " + path.string());
  }

  std::string line;
  while (std::getline(in, line)) {
    const auto commentPos = line.find('#');
    const auto content = (commentPos == std::string::npos)
                             ? line
                             : line.substr(0, commentPos);

    const auto eq = content.find('=');
    if (eq == std::string::npos) {
      continue;
    }

    const auto key = trim(content.substr(0, eq));
    const auto value = trim(content.substr(eq + 1));

    if (key == "arena.session.port") {
      config.arena.port =
          static_cast<uint16_t>(std::stoul(std::string(value)));
    } else if (key == "arena.session.host") {
      config.arena.host = std::string(value);
    } else if (key == "heap.session.port") {
      config.heap.port =
          static_cast<uint16_t>(std::stoul(std::string(value)));
    } else if (key == "heap.session.host") {
      config.heap.host = std::string(value);
    }
  }

  if (config.arena.port == 0 || config.heap.port == 0) {
    throw std::runtime_error(
        "server config must specify both arena.session.port and "
        "heap.session.port");
  }

  return config;
}

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle InvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle InvalidSocket = -1;
#endif

static void closeSocket(SocketHandle socket) {
  if (socket == InvalidSocket) {
    return;
  }
#ifdef _WIN32
  closesocket(socket);
#else
  ::close(socket);
#endif
}

#ifdef _WIN32
class WinSockInitializer {
 public:
  WinSockInitializer() {
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::system_error(WSAGetLastError(), std::system_category(),
                              "WSAStartup failed");
    }
  }

  ~WinSockInitializer() { WSACleanup(); }
};
#endif

class TcpServer {
 public:
  TcpServer(const std::string& bindHost, uint16_t port) {
#ifdef _WIN32
    static WinSockInitializer winsock;
#endif

    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == InvalidSocket) {
      throw std::system_error(lastSocketError(),
                              "failed to create listen socket");
    }

    int enable = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&enable), sizeof(enable));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr = resolveBindAddress(bindHost);

    if (::bind(listener_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
      closeSocket(listener_);
      throw std::system_error(lastSocketError(), "bind failed");
    }

    if (::listen(listener_, SOMAXCONN) != 0) {
      closeSocket(listener_);
      throw std::system_error(lastSocketError(), "listen failed");
    }
  }

  ~TcpServer() { closeSocket(listener_); }

  template <typename Handler>
  void serve(Handler handler) const {
    while (true) {
      sockaddr_in clientAddress{};
      socklen_t addressLength = sizeof(clientAddress);
      const SocketHandle client =
          ::accept(listener_, reinterpret_cast<sockaddr*>(&clientAddress),
                   &addressLength);

      if (client == InvalidSocket) {
        std::cerr << "accept failed: " << lastSocketError().message()
                  << '\n';
        continue;
      }

      try {
        processClient(client, handler);
      } catch (const std::exception& ex) {
        std::cerr << "client handler error: " << ex.what() << '\n';
      }

      closeSocket(client);
    }
  }

 private:
  static in_addr resolveBindAddress(const std::string& host) {
    if (host.empty()) {
      in_addr any{};
      any.s_addr = INADDR_ANY;
      return any;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (rc != 0) {
      throw std::system_error({rc, std::generic_category()},
                              "getaddrinfo failed for host: " + host);
    }

    if (!result) {
      in_addr any{};
      any.s_addr = INADDR_ANY;
      return any;
    }

    const auto address =
        reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return address;
  }

  template <typename Handler>
  static void processClient(SocketHandle client, Handler handler) {
    std::string backlog;
    backlog.reserve(4096);
    std::array<char, 4096> buffer;

    while (true) {
      const int bytes =
          ::recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (bytes <= 0) {
        break;
      }

      backlog.append(buffer.data(), bytes);

      size_t newlinePos;
      while ((newlinePos = backlog.find('\n')) != std::string::npos) {
        std::string message = backlog.substr(0, newlinePos);
        backlog.erase(0, newlinePos + 1);

        if (!message.empty() && message.back() == '\r') {
          message.pop_back();
        }

        if (!message.empty()) {
          handler(message);
        }
      }
    }

    if (!backlog.empty()) {
      if (backlog.back() == '\r') {
        backlog.pop_back();
      }
      if (!backlog.empty()) {
        handler(backlog);
      }
    }
  }

  static std::error_code lastSocketError() {
#ifdef _WIN32
    return {WSAGetLastError(), std::system_category()};
#else
    return {errno, std::system_category()};
#endif
  }

  SocketHandle listener_{InvalidSocket};
};

template <typename Processor>
void runServerSession(const std::string& name,
                      const SessionEndpoint& endpoint,
                      const std::filesystem::path& streamPath,
                      const std::filesystem::path& rawPath,
                      Processor processor) {
  using namespace std::chrono_literals;

  std::ofstream streamOut(streamPath, std::ios::trunc);
  std::ofstream rawOut(rawPath, std::ios::trunc);

  std::mutex rawMutex;
  std::atomic<std::uint64_t> count{0};

  std::thread heartbeat([&]() {
    while (true) {
      std::this_thread::sleep_for(5s);
      std::lock_guard<std::mutex> lock(rawMutex);
      const auto now = std::chrono::system_clock::now();
      rawOut << '<' << formatLocalTimeMs(now)
             << ">Heartbeat : processed " << count.load()
             << " messages, waiting for new messages" << '\n';
      rawOut.flush();
      streamOut.flush();
    }
  });
  heartbeat.detach();

  TcpServer server(endpoint.host, endpoint.port);

  std::cout << name << " session listening on "
            << (endpoint.host.empty() ? "0.0.0.0" : endpoint.host) << ':'
            << endpoint.port << " writing to " << streamPath << '\n';

  server.serve([&](const std::string& message) {
    const auto now = std::chrono::system_clock::now();
    {
      std::lock_guard<std::mutex> lock(rawMutex);
      rawOut << '<' << formatLocalTimeMs(now) << ">Incoming : " << message
             << '\n';
    }
    ++count;
    const auto ts = formatTimestamp(now);
    processor(message, ts, streamOut);
  });
}

}  // namespace

void StandaloneMode::run() const {
  const auto messages = loadFixMessages(config_.inputFile);
  if (messages.empty()) {
    std::cout << "no messages found in " << config_.inputFile << '\n';
    return;
  }

  demo::ArenaAllocator arena(256 * 1024);

    const auto arenaRawPath =
      config_.outputDir /
      (allocatorLabel(AllocatorPolicy::Arena) + std::string("_raw.log"));
    const auto heapRawPath =
      config_.outputDir /
      (allocatorLabel(AllocatorPolicy::Heap) + std::string("_raw.log"));


  benchmark(messages,
            [&](const std::string& msg) { processWithArena(msg, arena); },
            "Arena");

  benchmark(messages,
            [&](const std::string& msg) { processWithNew(msg); }, "Heap");

  const auto arenaBatch = config_.outputDir / "arena_batch.json";
  const auto heapBatch = config_.outputDir / "heap_batch.json";

  std::ofstream arenaRaw(arenaRawPath, std::ios::trunc);
  std::ofstream arenaWriter(arenaBatch, std::ios::trunc);
  for (const auto& message : messages) {
    const auto now = std::chrono::system_clock::now();
    const auto tsLocal = formatLocalTimeMs(now);
    const auto ts = formatTimestamp(now);
    arenaRaw << '<' << tsLocal << ">Incoming : " << message << '\n';
    processWithArena(message, arena, &arenaWriter, ts);
  }

  std::ofstream heapRaw(heapRawPath, std::ios::trunc);
  std::ofstream heapWriter(heapBatch, std::ios::trunc);
  for (const auto& message : messages) {
    const auto now = std::chrono::system_clock::now();
    const auto tsLocal = formatLocalTimeMs(now);
    const auto ts = formatTimestamp(now);
    heapRaw << tsLocal << " Incoming : " << message << '\n';
    processWithNew(message, &heapWriter, ts);
  }
}

void ServerMode::run() const {
  const auto serverConfig = loadServerConfig(config_.serverConfig);
  const auto arenaEndpoint = serverConfig.arena;
  const auto heapEndpoint = serverConfig.heap;

  const auto arenaPath =
      config_.outputDir /
      (allocatorLabel(AllocatorPolicy::Arena) + std::string("_stream.json"));
  const auto heapPath =
      config_.outputDir /
      (allocatorLabel(AllocatorPolicy::Heap) + std::string("_stream.json"));

  const auto arenaRawPath =
      config_.outputDir /
      (allocatorLabel(AllocatorPolicy::Arena) + std::string("_raw.log"));
  const auto heapRawPath =
      config_.outputDir /
      (allocatorLabel(AllocatorPolicy::Heap) + std::string("_raw.log"));

  std::thread arenaThread([&, arenaEndpoint, arenaPath]() {
    demo::ArenaAllocator arena(256 * 1024);
    runServerSession("Arena", arenaEndpoint, arenaPath, arenaRawPath,
                     [&](const std::string& msg, const std::string& ts,
                         std::ostream& out) {
                       processWithArena(msg, arena, &out, ts);
                     });
  });

  std::thread heapThread([&, heapEndpoint, heapPath]() {
    runServerSession("Heap", heapEndpoint, heapPath, heapRawPath,
                     [&](const std::string& msg, const std::string& ts,
                         std::ostream& out) {
                       processWithNew(msg, &out, ts);
                     });
  });

  arenaThread.join();
  heapThread.join();
}

std::unique_ptr<RunMode> makeRunMode(const RunConfig& config) {
  if (config.mode == Mode::Standalone) {
    return std::make_unique<StandaloneMode>(config);
  }

  return std::make_unique<ServerMode>(config);
}
