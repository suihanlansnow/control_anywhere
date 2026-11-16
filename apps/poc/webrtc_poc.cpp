// WebRTC 传输可行性 POC（libdatachannel）
//
// 目标：在不改动现有采集/编码/解码的前提下，验证三件事
//   1. 现有 H.264 编码器输出（Annex-B）能否经 WebRTC RTP 打包/解包后被现有解码器解出；
//   2. 端到端延迟（编码完成 → 解码出图）与码率是否满足远程桌面需求；
//   3. DataChannel 能否承载输入事件（本 POC 以回环 RTT 近似衡量）。
//
// 做法：单进程内建立两个 PeerConnection，通过回调直接交换 SDP/ICE（等价于信令），
// 网络走 127.0.0.1 的 host candidate，不需要 STUN/TURN。
//
// 用法：
//   rc-webrtc-poc [--capture synthetic|dxgi] [--fps 30] [--seconds 8] [--quality 70]

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "capture/capture.h"
#include "codec/decoder.h"
#include "codec/encoder.h"
#include "rtc/rtc.hpp"

#ifdef _WIN32
#include <objbase.h>
#endif

namespace {

using SteadyClock = std::chrono::steady_clock;

int64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             SteadyClock::now().time_since_epoch())
      .count();
}

double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  const std::size_t index =
      static_cast<std::size_t>(p * static_cast<double>(values.size() - 1));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

double average(const std::vector<double>& values) {
  if (values.empty()) return 0.0;
  double sum = 0.0;
  for (double v : values) sum += v;
  return sum / static_cast<double>(values.size());
}

void print_sdp_summary(const char* tag, const std::string& sdp) {
  std::size_t pos = 0;
  while (pos < sdp.size()) {
    const std::size_t end = sdp.find("\r\n", pos);
    const std::string line =
        sdp.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    if (line.rfind("m=", 0) == 0 || line.find("H264") != std::string::npos ||
        line.rfind("a=mid", 0) == 0 || line.rfind("a=send", 0) == 0 ||
        line.rfind("a=recv", 0) == 0 || line.rfind("a=inactive", 0) == 0) {
      std::printf("[%s] %s\n", tag, line.c_str());
    }
    if (end == std::string::npos) break;
    pos = end + 2;
  }
}

struct Stats {
  std::atomic<int> sent_frames{0};
  std::atomic<int> recv_frames{0};
  std::atomic<int> decoded_frames{0};
  std::atomic<int64_t> sent_bytes{0};
  std::mutex mutex;
  std::vector<double> e2e_ms;
  std::vector<double> decode_ms;
  std::vector<double> rtt_ms;
};

// 发送端为每帧的 RTP 时间戳记录发送时刻，接收端据此算端到端延迟；
// 用时间戳而不是序号，丢帧时不会错位。
struct TimestampIndex {
  void add(uint32_t ts, int64_t us) {
    std::lock_guard<std::mutex> lock(mutex);
    items_[ts] = us;
    while (items_.size() > 600) items_.erase(items_.begin());
  }

  int64_t find(uint32_t ts) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = items_.find(ts);
    return it == items_.end() ? -1 : it->second;
  }

 private:
  std::mutex mutex;
  std::map<uint32_t, int64_t> items_;
};

// 合成采集只输出 BGRA；POC 里做一次最简 BT.601 转换，真实链路由采集侧直接给 NV12。
bool bgra_to_nv12(const rc::CapturedFrame& src, rc::CapturedFrame& dst) {
  const int width = src.width;
  const int height = src.height & ~1;
  if (width <= 0 || height <= 0 || src.pixels.size() < static_cast<std::size_t>(src.stride) * static_cast<std::size_t>(src.height)) {
    return false;
  }
  dst.width = width;
  dst.height = height;
  dst.stride = width;
  dst.capture_ts_us = src.capture_ts_us;
  dst.format = rc::PixelFormat::kNV12;
  dst.pixels.resize(static_cast<std::size_t>(width) * height * 3 / 2);
  uint8_t* y_plane = dst.pixels.data();
  uint8_t* uv_plane = y_plane + static_cast<std::size_t>(width) * height;
  for (int y = 0; y < height; ++y) {
    const uint8_t* row = src.pixels.data() + static_cast<std::size_t>(y) * src.stride;
    for (int x = 0; x < width; ++x) {
      const int b = row[x * 4 + 0];
      const int g = row[x * 4 + 1];
      const int r = row[x * 4 + 2];
      y_plane[static_cast<std::size_t>(y) * width + x] =
          static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
    }
  }
  for (int y = 0; y < height; y += 2) {
    const uint8_t* row0 = src.pixels.data() + static_cast<std::size_t>(y) * src.stride;
    const uint8_t* row1 = row0 + src.stride;
    uint8_t* uv = uv_plane + static_cast<std::size_t>(y / 2) * width;
    for (int x = 0; x < width; x += 2) {
      int u_sum = 0;
      int v_sum = 0;
      for (int dy = 0; dy < 2; ++dy) {
        const uint8_t* row = dy == 0 ? row0 : row1;
        for (int dx = 0; dx < 2; ++dx) {
          const int b = row[(x + dx) * 4 + 0];
          const int g = row[(x + dx) * 4 + 1];
          const int r = row[(x + dx) * 4 + 2];
          u_sum += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
          v_sum += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
        }
      }
      uv[x] = static_cast<uint8_t>((u_sum + 2) / 4);
      uv[x + 1] = static_cast<uint8_t>((v_sum + 2) / 4);
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string capture_kind = "synthetic";
  int fps = 30;
  int seconds = 8;
  int quality = 70;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_next = (i + 1) < argc;
    if (arg == "--capture" && has_next) {
      capture_kind = argv[++i];
    } else if (arg == "--fps" && has_next) {
      fps = std::atoi(argv[++i]);
    } else if (arg == "--seconds" && has_next) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--quality" && has_next) {
      quality = std::atoi(argv[++i]);
    } else {
      std::printf("usage: %s [--capture synthetic|dxgi] [--fps N] [--seconds N] "
                  "[--quality N]\n",
                  argv[0]);
      return 2;
    }
  }
  if (fps <= 0) fps = 30;
  if (seconds <= 0) seconds = 8;

  rtc::InitLogger(rtc::LogLevel::Warning);

  Stats stats;
  TimestampIndex send_times;
  std::atomic<bool> pc_connected{false};
  std::atomic<bool> dc_open{false};
  std::atomic<bool> track_open{false};
  std::atomic<int> rtt_sent{0};
  std::atomic<int> rtt_recv{0};

  // 两个 PeerConnection 通过回调直连，等价于一条本地信令通道。
  auto config = rtc::Configuration();
  auto sender = std::make_shared<rtc::PeerConnection>(config);
  auto receiver = std::make_shared<rtc::PeerConnection>(config);

  sender->onLocalDescription([receiver](rtc::Description description) {
    receiver->setRemoteDescription(description);
  });
  sender->onLocalCandidate([receiver](rtc::Candidate candidate) {
    receiver->addRemoteCandidate(candidate);
  });
  receiver->onLocalDescription([sender](rtc::Description description) {
    std::printf("[poc] receiver local description: %s\n", description.typeString().c_str());
    print_sdp_summary("sdp-answer", description.generateSdp());
    sender->setRemoteDescription(description);
  });
  receiver->onLocalCandidate([sender](rtc::Candidate candidate) {
    sender->addRemoteCandidate(candidate);
  });

  sender->onStateChange([&pc_connected](rtc::PeerConnection::State state) {
    if (state == rtc::PeerConnection::State::Connected) pc_connected.store(true);
    std::printf("[poc] sender state: %d\n", static_cast<int>(state));
  });
  receiver->onStateChange([](rtc::PeerConnection::State state) {
    std::printf("[poc] receiver state: %d\n", static_cast<int>(state));
  });

  // ---- 视频轨：H.264，载荷类型 96，SSRC 42 ------------------------------------
  const rtc::SSRC kSsrc = 42;
  const uint8_t kPayloadType = 96;
  rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
  video.addH264Codec(kPayloadType);
  video.addSSRC(kSsrc, "video", "poc", "video");
  auto video_track = sender->addTrack(video);

  auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
      kSsrc, "video", kPayloadType, rtc::H264RtpPacketizer::ClockRate);
  auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
      rtc::NalUnit::Separator::StartSequence, rtp_config);
  auto sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
  packetizer->addToChain(sr_reporter);
  packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
  video_track->setMediaHandler(packetizer);
  video_track->onOpen([&track_open] {
    std::printf("[poc] video track open\n");
    track_open.store(true);
  });

  // ---- 输入通道：DataChannel ------------------------------------------------
  std::shared_ptr<rtc::DataChannel> remote_dc;  // 同样要持有，避免被回收
  auto input_dc = sender->createDataChannel("input");
  receiver->onDataChannel([&remote_dc](std::shared_ptr<rtc::DataChannel> dc) {
    remote_dc = dc;
    dc->onMessage([dc](rtc::message_variant data) {
      if (auto* binary = std::get_if<rtc::binary>(&data)) {
        dc->send(*binary);  // 回环：原样回送，用于测 RTT
      }
    });
  });
  input_dc->onOpen([&dc_open] {
    std::printf("[poc] data channel open\n");
    dc_open.store(true);
  });
  input_dc->onMessage([&stats, &rtt_recv](rtc::message_variant data) {
    if (auto* binary = std::get_if<rtc::binary>(&data)) {
      if (binary->size() < 16) return;
      int64_t sent_us = 0;
      std::memcpy(&sent_us, binary->data() + 8, sizeof(sent_us));
      {
        std::lock_guard<std::mutex> lock(stats.mutex);
        stats.rtt_ms.push_back(static_cast<double>(now_us() - sent_us) / 1000.0);
      }
      rtt_recv.fetch_add(1);
    }
  });

  // 接收侧：解包 → 解码线程。
  // 注意必须持有 remote Track 的 shared_ptr：libdatachannel 内部只存 weak_ptr，
  // 回调返回后若无人持有，重新生成 SDP 时该媒体行会被标记为 removed（m=video 0）。
  struct DecodeTask {
    rtc::binary data;
    uint32_t ts = 0;
    int64_t sent_at = -1;
  };
  std::mutex decode_mutex;
  std::condition_variable decode_cv;
  std::deque<DecodeTask> decode_queue;
  bool decode_stop = false;
  std::atomic<int> decode_dropped{0};
  std::string decoder_name = "none";
  std::atomic<int> decoded_width{0};
  std::atomic<int> decoded_height{0};

  std::shared_ptr<rtc::Track> receiver_track;
  receiver->onTrack([&](std::shared_ptr<rtc::Track> track) {
    std::printf("[poc] receiver got track mid=%s\n", track->mid().c_str());
    receiver_track = track;
    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
        rtc::NalUnit::Separator::StartSequence);
    depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track->setMediaHandler(depacketizer);
    track->onFrame([&](rtc::binary data, rtc::FrameInfo info) {
      // 回调跑在 libdatachannel 的网络线程上，绝不能在这里解码（会阻塞 RTP/DataChannel）。
      const int64_t sent_at = send_times.find(info.timestamp);
      stats.recv_frames.fetch_add(1);
      std::lock_guard<std::mutex> lock(decode_mutex);
      if (decode_queue.size() >= 3) {
        decode_queue.pop_front();
        decode_dropped.fetch_add(1);
      }
      decode_queue.push_back(DecodeTask{std::move(data), info.timestamp, sent_at});
      decode_cv.notify_one();
    });
  });

  // 解码线程：与现有主控端一致，网络线程只管收，解码单独一条线程。
  std::thread decode_thread([&]() {
#ifdef _WIN32
    // Media Foundation 解码器要求调用线程初始化过 COM。
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
    std::string decoder_error;
    auto decoder = rc::create_decoder("h264", &decoder_error);
    if (!decoder || !decoder->open(&decoder_error)) {
      std::printf("[poc] decoder open failed: %s\n", decoder_error.c_str());
      return;
    }
    decoder_name = decoder->name();
    while (true) {
      DecodeTask task;
      {
        std::unique_lock<std::mutex> lock(decode_mutex);
        decode_cv.wait(lock, [&] { return decode_stop || !decode_queue.empty(); });
        if (decode_queue.empty()) break;  // stop 且队列已空
        task = std::move(decode_queue.front());
        decode_queue.pop_front();
      }
      rc::DecodedImage image;
      const int64_t decode_start = now_us();
      if (!decoder->decode(reinterpret_cast<const uint8_t*>(task.data.data()), task.data.size(),
                           static_cast<int64_t>(task.ts), image, &decoder_error)) {
        continue;
      }
      const double decode_ms = static_cast<double>(now_us() - decode_start) / 1000.0;
      // 端到端：编码完成（send_us）→ 解码出图（decode 返回后）。
      const int64_t decoded_at = now_us();

      stats.decoded_frames.fetch_add(1);
      decoded_width.store(image.width);
      decoded_height.store(image.height);
      {
        std::lock_guard<std::mutex> stats_lock(stats.mutex);
        stats.decode_ms.push_back(decode_ms);
        if (task.sent_at > 0) {
          stats.e2e_ms.push_back(static_cast<double>(decoded_at - task.sent_at) / 1000.0);
        }
      }
    }
    decoder->close();
  });

  // ---- 采集 + 编码 ----------------------------------------------------------
  std::string error;
  auto capture = rc::create_capture_source(capture_kind, &error);
  if (!capture || !capture->start(0, &error)) {
    std::printf("[poc] capture '%s' failed: %s\n", capture_kind.c_str(), error.c_str());
    return 1;
  }
  auto encoder = rc::create_encoder("h264", &error);
  if (!encoder) {
    std::printf("[poc] encoder unavailable: %s\n", error.c_str());
    return 1;
  }

  rtp_config->startTimestamp = static_cast<uint32_t>(now_us() * 90 / 1000);
  rtp_config->timestamp = rtp_config->startTimestamp;
  sender->setLocalDescription();
  if (auto local = sender->localDescription()) {
    const std::string sdp = local->generateSdp();
    std::size_t pos = 0;
    while (pos < sdp.size()) {
      const std::size_t end = sdp.find("\r\n", pos);
      const std::string line =
          sdp.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      if (line.rfind("m=", 0) == 0 || line.find("H264") != std::string::npos ||
          line.rfind("a=mid", 0) == 0 || line.rfind("a=send", 0) == 0) {
        std::printf("[sdp] %s\n", line.c_str());
      }
      if (end == std::string::npos) break;
      pos = end + 2;
    }
  }
  std::printf("[poc] video track open=%d\n", video_track->isOpen() ? 1 : 0);

  const auto started = SteadyClock::now();
  auto last_time = started;
  uint32_t frames_encoded = 0;
  uint32_t frames_grabbed = 0;
  int consecutive_encode_failures = 0;
  bool encoder_opened = false;

  while (true) {
    const double elapsed =
        std::chrono::duration<double>(SteadyClock::now() - started).count();
    if (elapsed >= seconds) break;

    rc::CapturedFrame frame;
    if (!capture->grab(frame, 33)) continue;
    ++frames_grabbed;

    if (!encoder_opened) {
      if (!encoder->open(frame.width, frame.height, fps, quality, &error)) {
        std::printf("[poc] encoder open failed: %s\n", error.c_str());
        return 1;
      }
      encoder_opened = true;
      std::printf("[poc] stream %dx%d @%dfps\n", frame.width, frame.height, fps);
    }

    rc::EncodedFrame encoded;
    rc::CapturedFrame nv12;
    const rc::CapturedFrame* input = &frame;
    if (frame.format == rc::PixelFormat::kBGRA) {
      if (!bgra_to_nv12(frame, nv12)) continue;
      input = &nv12;
    }
    if (!encoder->encode(*input, encoded, &error)) {
      // Media Foundation 异步 MFT 前几帧可能还没出图，与现有被控端一致：告警后继续。
      if (consecutive_encode_failures++ < 60) {
        if (consecutive_encode_failures <= 3) {
          std::printf("[poc] encode failed (%d): %s\n", consecutive_encode_failures,
                      error.c_str());
        }
        continue;
      }
      std::printf("[poc] encode failed %d times, giving up: %s\n",
                  consecutive_encode_failures, error.c_str());
      break;
    }
    consecutive_encode_failures = 0;
    if (encoded.data.empty()) continue;

    const int64_t send_us = now_us();
    const double since_last =
        std::chrono::duration<double>(SteadyClock::now() - last_time).count();
    last_time = SteadyClock::now();
    rtp_config->timestamp += static_cast<uint32_t>(since_last * 90000.0);
    if (frames_encoded % fps == 0) sr_reporter->setNeedsToReport();

    // 不依赖 onOpen 回调：send() 内部会校验 Track 是否已建立 DTLS-SRTP，
    // 未就绪时抛异常，这里吞掉即可，等下一帧再发。
    send_times.add(rtp_config->timestamp, send_us);
    try {
      video_track->send(reinterpret_cast<const std::byte*>(encoded.data.data()),
                        encoded.data.size());
      stats.sent_frames.fetch_add(1);
      stats.sent_bytes.fetch_add(static_cast<int64_t>(encoded.data.size()));
      if (!track_open.load()) {
        track_open.store(true);
        std::printf("[poc] video track ready (first frame sent)\n");
      }
    } catch (const std::exception& e) {
      if (elapsed < 1.0) std::printf("[poc] send deferred: %s\n", e.what());
    }

    // DataChannel 输入事件：每 5 帧发一次 16 字节消息（序号 + 发送时刻）。
    if (dc_open.load() && (frames_encoded % 5) == 0) {
      rtc::binary payload(16);
      const int seq = rtt_sent.fetch_add(1);
      std::memcpy(payload.data(), &seq, sizeof(seq));
      const int64_t ts = now_us();
      std::memcpy(payload.data() + 8, &ts, sizeof(ts));
      try {
        input_dc->send(payload);
      } catch (const std::exception&) {
      }
    }
    ++frames_encoded;
  }

  const double total_seconds =
      std::chrono::duration<double>(SteadyClock::now() - started).count();
  capture->stop();
  encoder->close();
  {
    std::lock_guard<std::mutex> lock(decode_mutex);
    decode_stop = true;
  }
  decode_cv.notify_all();
  if (decode_thread.joinable()) decode_thread.join();

  // ---- 结果 ----------------------------------------------------------------
  const int sent = stats.sent_frames.load();
  const int recv = stats.recv_frames.load();
  const int decoded = stats.decoded_frames.load();
  const int dropped = decode_dropped.load();
  const double bitrate_mbps =
      static_cast<double>(stats.sent_bytes.load()) * 8.0 / total_seconds / 1e6;

  std::printf("\n==== WebRTC POC 结果 ====\n");
  std::printf("采集/编码: %s / h264  解码: %s  输出: %dx%d\n", capture_kind.c_str(),
              decoder_name.c_str(), decoded_width.load(), decoded_height.load());
  std::printf("ICE/DTLS 已连接: %s  视频轨: %s  DataChannel: %s\n",
              pc_connected.load() ? "yes" : "no", track_open.load() ? "open" : "closed",
              dc_open.load() ? "open" : "closed");
  std::printf("帧: 采集 %u, 发出 %d, 收到 %d, 解码 %d (%.1f%%), 解码队列丢帧 %d\n",
              frames_grabbed, sent, recv, decoded, sent > 0 ? 100.0 * decoded / sent : 0.0,
              dropped);
  std::printf("码率: %.2f Mbps (H.264 payload)\n", bitrate_mbps);

  double e2e_avg = 0.0, e2e_p95 = 0.0, e2e_max = 0.0;
  double decode_avg = 0.0, rtt_avg = 0.0, rtt_p95 = 0.0;
  {
    std::lock_guard<std::mutex> lock(stats.mutex);
    e2e_avg = average(stats.e2e_ms);
    e2e_p95 = percentile(stats.e2e_ms, 0.95);
    e2e_max = percentile(stats.e2e_ms, 1.0);
    decode_avg = average(stats.decode_ms);
    rtt_avg = average(stats.rtt_ms);
    rtt_p95 = percentile(stats.rtt_ms, 0.95);
  }
  std::printf("端到端延迟(发送→解码出图): avg %.2f ms, p95 %.2f ms, max %.2f ms\n",
              e2e_avg, e2e_p95, e2e_max);
  std::printf("解码耗时: avg %.2f ms\n", decode_avg);
  std::printf("DataChannel RTT: avg %.2f ms, p95 %.2f ms, 样本 %d\n", rtt_avg, rtt_p95,
              static_cast<int>(rtt_recv.load()));

  const bool pass = pc_connected.load() && track_open.load() && dc_open.load() &&
                    sent > 0 && decoded >= sent * 9 / 10 && e2e_avg < 300.0;
  std::printf("结论: %s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
