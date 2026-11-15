#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "net/socket.h"

namespace rc::web {

struct HttpRequest {
  std::string method;
  std::string path;
  std::string query;
  std::string body;
  // 头部名统一小写，值保留原样。
  std::map<std::string, std::string> headers;

  // 取查询参数，未找到返回空串。
  std::string query_param(const std::string& name) const;
};

// 处理函数自己往 socket 写完整响应；返回 true 表示保持连接读取下一个请求。
using HttpHandler = std::function<bool(const HttpRequest&, TcpSocket&)>;
// 全局请求过滤器：返回 false 表示已写响应并跳过路由（用于鉴权）。
using HttpFilter = std::function<bool(const HttpRequest&, TcpSocket&)>;

bool http_write_response(TcpSocket& socket, int status, const char* content_type,
                         const std::string& body,
                         const std::string& extra_headers = std::string());

// 只支持本场景需要的子集：HTTP/1.1、GET/POST、Content-Length、keep-alive。
// 不解析 chunked 与 multipart 请求体。
class HttpServer {
 public:
  HttpServer() = default;
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  void route(std::string path, HttpHandler handler);
  void set_filter(HttpFilter filter) { filter_ = std::move(filter); }
  bool listen(uint16_t port, std::string* error);
  bool listen(const std::string& bind_address, uint16_t port, std::string* error);
  // 阻塞在 accept 上，直到 stop() 被调用。
  void run();
  void stop();

 private:
  void handle_connection(TcpSocket socket);
  bool read_request(TcpSocket& socket, std::string& buffer, HttpRequest& request);

  std::map<std::string, HttpHandler> routes_;
  std::mutex routes_mutex_;
  HttpFilter filter_;
  TcpListener listener_;
  std::atomic<bool> stop_{false};
};

}  // namespace rc::web
