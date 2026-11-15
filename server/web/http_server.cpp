#include "http_server.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <vector>

#include "base/log.h"

namespace rc::web {
namespace {

const char* status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    default: return "OK";
  }
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string trim(const std::string& text) {
  const std::size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const std::size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

}  // namespace

std::string HttpRequest::query_param(const std::string& name) const {
  std::size_t pos = 0;
  while (pos <= query.size()) {
    const std::size_t amp = query.find('&', pos);
    const std::string pair =
        query.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
    const std::size_t eq = pair.find('=');
    const std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
    if (key == name) return eq == std::string::npos ? std::string() : pair.substr(eq + 1);
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return {};
}

bool http_write_response(TcpSocket& socket, int status, const char* content_type,
                         const std::string& body, const std::string& extra_headers) {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n";
  head += "Content-Type: ";
  head += content_type;
  head += "\r\nContent-Length: " + std::to_string(body.size());
  head += "\r\nCache-Control: no-store\r\nConnection: keep-alive\r\n";
  if (!extra_headers.empty()) {
    head += extra_headers;
    if (head.size() < 2 || head.compare(head.size() - 2, 2, "\r\n") != 0) head += "\r\n";
  }
  head += "\r\n";
  if (!socket.send_all(head.data(), head.size())) return false;
  return body.empty() || socket.send_all(body.data(), body.size());
}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(std::string path, HttpHandler handler) {
  std::lock_guard<std::mutex> lock(routes_mutex_);
  routes_[std::move(path)] = std::move(handler);
}

bool HttpServer::listen(uint16_t port, std::string* error) {
  listener_ = TcpListener::listen(port, error);
  return listener_.valid();
}

bool HttpServer::listen(const std::string& bind_address, uint16_t port,
                        std::string* error) {
  listener_ = TcpListener::listen(bind_address, port, error);
  return listener_.valid();
}

void HttpServer::stop() {
  stop_.store(true);
  listener_.close();
}

void HttpServer::run() {
  while (!stop_.load()) {
    TcpSocket socket = listener_.accept();
    if (!socket.valid()) {
      if (stop_.load()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    // 每个浏览器连接一个线程：连接数很少，保持实现简单。
    std::thread([this, sock = std::move(socket)]() mutable {
      handle_connection(std::move(sock));
    }).detach();
  }
}

void HttpServer::handle_connection(TcpSocket socket) {
  // 空闲连接最多保持 60s，避免浏览器关闭标签后线程永久堆积。
  socket.set_recv_timeout_ms(60000);
  socket.set_send_timeout_ms(15000);

  std::string buffer;
  while (!stop_.load()) {
    HttpRequest request;
    if (!read_request(socket, buffer, request)) break;

    // 鉴权过滤器：不合法的请求由过滤器自行写 401/302，不进入路由。
    if (filter_ && !filter_(request, socket)) continue;

    HttpHandler handler;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      auto it = routes_.find(request.path);
      if (it != routes_.end()) handler = it->second;
    }
    if (!handler) {
      http_write_response(socket, 404, "text/plain; charset=utf-8", "not found");
      continue;
    }
    if (!handler(request, socket)) break;
  }
  socket.close();
}

bool HttpServer::read_request(TcpSocket& socket, std::string& buffer,
                              HttpRequest& request) {
  std::size_t header_end = buffer.find("\r\n\r\n");
  while (header_end == std::string::npos) {
    char chunk[4096];
    const int got = socket.recv_some(chunk, sizeof(chunk));
    if (got <= 0) return false;
    buffer.append(chunk, static_cast<std::size_t>(got));
    if (buffer.size() > (1u << 20)) return false;  // 头部超过 1MB 视为异常请求
    header_end = buffer.find("\r\n\r\n");
  }

  const std::string header_block = buffer.substr(0, header_end);

  std::istringstream stream(header_block);
  std::string request_line;
  std::getline(stream, request_line);
  if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

  std::istringstream line(request_line);
  std::string target;
  std::string version;
  if (!(line >> request.method >> target >> version)) return false;

  const std::size_t question = target.find('?');
  if (question == std::string::npos) {
    request.path = target;
  } else {
    request.path = target.substr(0, question);
    request.query = target.substr(question + 1);
  }

  std::size_t content_length = 0;
  std::string header;
  while (std::getline(stream, header)) {
    if (!header.empty() && header.back() == '\r') header.pop_back();
    const std::size_t colon = header.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = lower(trim(header.substr(0, colon)));
    const std::string value = trim(header.substr(colon + 1));
    request.headers[name] = value;
    if (name == "content-length") {
      content_length = static_cast<std::size_t>(std::strtoul(value.c_str(), nullptr, 10));
    }
  }

  const std::size_t body_start = header_end + 4;
  while (buffer.size() < body_start + content_length) {
    char chunk[4096];
    const int got = socket.recv_some(chunk, sizeof(chunk));
    if (got <= 0) return false;
    buffer.append(chunk, static_cast<std::size_t>(got));
  }
  request.body = buffer.substr(body_start, content_length);
  buffer.erase(0, body_start + content_length);
  return true;
}

}  // namespace rc::web
