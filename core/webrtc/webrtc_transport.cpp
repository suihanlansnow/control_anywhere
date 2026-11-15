#include "webrtc/webrtc_transport.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

#include <rtc/rtc.hpp>

#include "base/clock.h"
#include "base/log.h"

namespace rc {
namespace {

// libdatachannel 的日志只需初始化一次，重复调用会覆盖等级，这里用 once 避免干扰。
void init_rtc_logger() {
  static std::once_flag flag;
  std::call_once(flag, [] { rtc::InitLogger(rtc::LogLevel::Warning); });
}

int64_t steady_now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

constexpr uint8_t kVideoPayloadType = 96;
constexpr rtc::SSRC kVideoSsrc = 42;

rtc::Configuration build_rtc_config(const WebRtcConfig& config) {
  rtc::Configuration rtc_config;
  for (const auto& uri : config.ice_servers) {
    if (uri.empty()) continue;
    try {
      rtc_config.iceServers.emplace_back(uri);
    } catch (const std::exception& e) {
      RC_LOGW("webrtc", "ignore invalid ICE server '" << uri << "': " << e.what());
    }
  }
  if (config.force_relay) {
    rtc_config.iceTransportPolicy = rtc::TransportPolicy::Relay;
    if (rtc_config.iceServers.empty()) {
      RC_LOGW("webrtc", "force_relay enabled but no TURN server configured");
    }
  }
  rtc_config.enableIceTcp = config.enable_ice_tcp;
  return rtc_config;
}

// 连接建立后打印实际选中的候选对（host/srflx/relay），弱网排查时很有用。
void log_selected_pair(const std::shared_ptr<rtc::PeerConnection>& pc, const char* role) {
  if (!pc) return;
  rtc::Candidate local;
  rtc::Candidate remote;
  if (pc->getSelectedCandidatePair(&local, &remote)) {
    RC_LOGI("webrtc", role << " selected pair local=" << local.candidate()
                           << " remote=" << remote.candidate());
  } else {
    RC_LOGI("webrtc", role << " connected (no selected pair reported)");
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// 被控端：offerer，推送 H.264 视频并提供输入 DataChannel。
// ---------------------------------------------------------------------------
struct WebRtcHost::Impl {
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track> video_track;
  std::shared_ptr<rtc::DataChannel> input_dc;
  std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config;
  std::shared_ptr<rtc::RtcpSrReporter> sr_reporter;
  std::shared_ptr<Callbacks> callbacks;
  std::atomic<bool> connected{false};
  std::atomic<bool> closed{false};

  int64_t start_us = 0;
  int64_t last_report_us = 0;
  std::mutex send_mutex;
};

WebRtcHost::WebRtcHost() : impl_(std::make_unique<Impl>()) {}
WebRtcHost::~WebRtcHost() { close(); }

bool WebRtcHost::start(const Callbacks& callbacks, const WebRtcConfig& config,
                       std::string* error) {
  init_rtc_logger();
  auto cb = std::make_shared<Callbacks>(callbacks);
  impl_->callbacks = cb;

  try {
    impl_->pc = std::make_shared<rtc::PeerConnection>(build_rtc_config(config));

    impl_->pc->onLocalDescription([cb](rtc::Description description) {
      if (cb->signaling.on_local_description) {
        cb->signaling.on_local_description(description.typeString(),
                                           description.generateSdp());
      }
    });
    impl_->pc->onLocalCandidate([cb](rtc::Candidate candidate) {
      if (cb->signaling.on_local_candidate) {
        cb->signaling.on_local_candidate(candidate.candidate(), candidate.mid());
      }
    });
    auto pc = impl_->pc;
    impl_->pc->onStateChange([this, pc](rtc::PeerConnection::State state) {
      RC_LOGI("webrtc", "host peer state=" << static_cast<int>(state));
      impl_->connected.store(state == rtc::PeerConnection::State::Connected);
      if (state == rtc::PeerConnection::State::Connected) log_selected_pair(pc, "host");
    });

    rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
    video.addH264Codec(kVideoPayloadType);
    video.addSSRC(kVideoSsrc, "video", "rc", "video");
    impl_->video_track = impl_->pc->addTrack(video);

    impl_->rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        kVideoSsrc, "video", kVideoPayloadType, rtc::H264RtpPacketizer::ClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
        rtc::NalUnit::Separator::StartSequence, impl_->rtp_config);
    impl_->sr_reporter = std::make_shared<rtc::RtcpSrReporter>(impl_->rtp_config);
    packetizer->addToChain(impl_->sr_reporter);
    packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
    impl_->video_track->setMediaHandler(packetizer);
    impl_->video_track->onOpen([] { RC_LOGI("webrtc", "video track open"); });

    impl_->input_dc = impl_->pc->createDataChannel("input");
    impl_->input_dc->onMessage([cb](rtc::message_variant data) {
      if (auto* binary = std::get_if<rtc::binary>(&data)) {
        if (cb->on_input) {
          cb->on_input(reinterpret_cast<const uint8_t*>(binary->data()),
                       binary->size());
        }
      }
    });

    // RTP 时间戳用单调时钟推导，避免系统时间跳变导致对端抖动。
    impl_->start_us = steady_now_us();
    impl_->rtp_config->startTimestamp =
        static_cast<uint32_t>(static_cast<uint64_t>(impl_->start_us) * 9 / 100);
    impl_->rtp_config->timestamp = impl_->rtp_config->startTimestamp;

    // 添加轨道/DataChannel 后 libdatachannel 会自动协商并生成 offer
    // （通过 onLocalDescription 回调送出），不要再显式调用 setLocalDescription。
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
  RC_LOGI("webrtc", "host started, offer generated");
  return true;
}

bool WebRtcHost::set_answer(const std::string& sdp, std::string* error) {
  if (!impl_->pc) {
    if (error) *error = "peer connection not started";
    return false;
  }
  try {
    impl_->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

void WebRtcHost::add_remote_candidate(const std::string& candidate,
                                      const std::string& mid) {
  if (!impl_->pc) return;
  try {
    impl_->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
  } catch (const std::exception& e) {
    RC_LOGW("webrtc", "add remote candidate failed: " << e.what());
  }
}

bool WebRtcHost::send_video(const uint8_t* data, std::size_t size) {
  if (!impl_->video_track || impl_->closed.load() || size == 0) return false;
  std::lock_guard<std::mutex> lock(impl_->send_mutex);
  const int64_t now = steady_now_us();
  const uint64_t elapsed_us = static_cast<uint64_t>(now - impl_->start_us);
  impl_->rtp_config->timestamp = impl_->rtp_config->startTimestamp +
                                 static_cast<uint32_t>(elapsed_us * 9 / 100);
  if (now - impl_->last_report_us >= 1000000) {
    impl_->sr_reporter->setNeedsToReport();
    impl_->last_report_us = now;
  }
  try {
    impl_->video_track->send(reinterpret_cast<const std::byte*>(data), size);
    return true;
  } catch (const std::exception&) {
    // DTLS-SRTP 尚未就绪或链路正在重建：丢弃本帧，下一次继续尝试。
    return false;
  }
}

bool WebRtcHost::connected() const { return impl_->connected.load(); }

void WebRtcHost::close() {
  if (impl_->closed.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(impl_->send_mutex);
    if (impl_->input_dc) impl_->input_dc->close();
    if (impl_->video_track) impl_->video_track->close();
    if (impl_->pc) impl_->pc->close();
    impl_->input_dc.reset();
    impl_->video_track.reset();
    impl_->pc.reset();
  }
}

// ---------------------------------------------------------------------------
// 主控端：answerer，接收视频轨并回送输入事件。
// ---------------------------------------------------------------------------
struct WebRtcClient::Impl {
  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track> video_track;  // 必须强引用，否则重新协商时媒体行会被移除。
  std::shared_ptr<rtc::DataChannel> input_dc;
  std::shared_ptr<Callbacks> callbacks;
  std::atomic<bool> connected{false};
  std::atomic<bool> closed{false};
  std::mutex send_mutex;
};

WebRtcClient::WebRtcClient() : impl_(std::make_unique<Impl>()) {}
WebRtcClient::~WebRtcClient() { close(); }

bool WebRtcClient::start(const Callbacks& callbacks, const WebRtcConfig& config,
                         std::string* error) {
  init_rtc_logger();
  auto cb = std::make_shared<Callbacks>(callbacks);
  impl_->callbacks = cb;

  try {
    impl_->pc = std::make_shared<rtc::PeerConnection>(build_rtc_config(config));
    impl_->pc->onLocalDescription([cb](rtc::Description description) {
      if (cb->signaling.on_local_description) {
        cb->signaling.on_local_description(description.typeString(),
                                           description.generateSdp());
      }
    });
    impl_->pc->onLocalCandidate([cb](rtc::Candidate candidate) {
      if (cb->signaling.on_local_candidate) {
        cb->signaling.on_local_candidate(candidate.candidate(), candidate.mid());
      }
    });
    auto pc = impl_->pc;
    impl_->pc->onStateChange([this, pc](rtc::PeerConnection::State state) {
      RC_LOGI("webrtc", "client peer state=" << static_cast<int>(state));
      impl_->connected.store(state == rtc::PeerConnection::State::Connected);
      if (state == rtc::PeerConnection::State::Connected) log_selected_pair(pc, "client");
    });

    impl_->pc->onTrack([this, cb](std::shared_ptr<rtc::Track> track) {
      RC_LOGI("webrtc", "video track received mid=" << track->mid());
      impl_->video_track = track;
      auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(
          rtc::NalUnit::Separator::StartSequence);
      depacketizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
      track->setMediaHandler(depacketizer);
      track->onFrame([cb](rtc::binary data, rtc::FrameInfo info) {
        // 回调运行在网络线程：调用方只能入队，不能同步解码。
        if (cb->on_video) {
          cb->on_video(reinterpret_cast<const uint8_t*>(data.data()), data.size(),
                       info.timestamp);
        }
      });
    });

    impl_->pc->onDataChannel([this](std::shared_ptr<rtc::DataChannel> dc) {
      RC_LOGI("webrtc", "input data channel received label=" << dc->label());
      impl_->input_dc = dc;
      dc->onOpen([] { RC_LOGI("webrtc", "input data channel open"); });
    });
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
  return true;
}

bool WebRtcClient::set_offer(const std::string& sdp, std::string* error) {
  if (!impl_->pc) {
    if (error) *error = "peer connection not started";
    return false;
  }
  try {
    impl_->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

void WebRtcClient::add_remote_candidate(const std::string& candidate,
                                        const std::string& mid) {
  if (!impl_->pc) return;
  try {
    impl_->pc->addRemoteCandidate(rtc::Candidate(candidate, mid));
  } catch (const std::exception& e) {
    RC_LOGW("webrtc", "add remote candidate failed: " << e.what());
  }
}

bool WebRtcClient::send_input(const uint8_t* data, std::size_t size) {
  if (!impl_->input_dc || impl_->closed.load()) return false;
  std::shared_ptr<rtc::DataChannel> dc;
  {
    std::lock_guard<std::mutex> lock(impl_->send_mutex);
    dc = impl_->input_dc;
  }
  if (!dc) return false;
  try {
    dc->send(rtc::binary(reinterpret_cast<const std::byte*>(data),
                         reinterpret_cast<const std::byte*>(data) + size));
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

bool WebRtcClient::connected() const { return impl_->connected.load(); }

void WebRtcClient::close() {
  if (impl_->closed.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(impl_->send_mutex);
    if (impl_->input_dc) impl_->input_dc->close();
    if (impl_->video_track) impl_->video_track->close();
    if (impl_->pc) impl_->pc->close();
    impl_->input_dc.reset();
    impl_->video_track.reset();
    impl_->pc.reset();
  }
}

}  // namespace rc
