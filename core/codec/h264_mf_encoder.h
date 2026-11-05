#pragma once

#ifdef _WIN32

#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "codec/encoder.h"

namespace rc {

// Media Foundation H.264 硬编（MFT）。
// 优先枚举硬件编码器（NVENC/QSV/AMF），失败再退回系统软编 MFT。
// 输入把采集的 BGRA 转成 NV12，输出 Annex-B 码流，全程内存态、零拷贝落盘。
class H264MfEncoder final : public IVideoEncoder {
 public:
  ~H264MfEncoder() override;

  bool open(int width, int height, int fps, int quality, std::string* error) override;
  void close() override;
  bool encode(const CapturedFrame& source, EncodedFrame& out,
              std::string* error) override;
  bool set_d3d_device(void* device) override;
  bool accepts_texture_input() const override { return texture_input_; }
  void* native_device_manager() override { return device_manager_.Get(); }
  const char* name() const override { return hardware_ ? "h264-mf-hw" : "h264-mf-sw"; }

 private:
  bool activate(bool hardware_only, std::string* error);
  bool configure(int width, int height, int fps, int quality, std::string* error);
  bool setup_d3d_manager();
  bool setup_sample_allocator();
  bool convert_nv12(const CapturedFrame& source, std::string* error);
  bool texture_sample(const std::shared_ptr<void>& texture, int width, int height,
                      Microsoft::WRL::ComPtr<IMFSample>& sample, std::string* error);
  // 把采集纹理拷进 MFT 样本的显存（持设备锁，和 MFT 的 GPU 工作串行）。
  bool copy_to_sample(const std::shared_ptr<void>& texture, IMFSample* sample,
                      std::string* error);
  bool accept_input(IMFSample* prepared, std::string* error);
  bool produce_output(EncodedFrame& out, int timeout_ms, std::string* error);
  // 等待指定事件；期间出现的 NeedInput / HaveOutput 记为令牌，避免事件被吞掉后死锁。
  bool wait_event(uint32_t want, int timeout_ms);
  bool renegotiate(std::string* error);

  Microsoft::WRL::ComPtr<IMFTransform> transform_;
  Microsoft::WRL::ComPtr<IMFMediaEventGenerator> events_;
  Microsoft::WRL::ComPtr<IUnknown> shared_device_;
  Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> device_manager_;
  // 输入纹理必须来自样本分配器；NVENC 不暴露自己的实现，用通用分配器兜底。
  Microsoft::WRL::ComPtr<IMFVideoSampleAllocatorEx> sample_allocator_;
  Microsoft::WRL::ComPtr<IMFMediaType> input_type_;
  HANDLE device_handle_ = nullptr;
  // 已送进 MFT、还没取回对应输出的样本；取回一帧输出就回收一个。
  std::deque<Microsoft::WRL::ComPtr<IMFSample>> held_samples_;
  std::size_t held_peak_ = 0;             // 观测到的在途样本峰值（= 硬编流水深度）
  int cpu_miss_ = 0;                      // 直通模式下连续收到 CPU 帧的次数
  std::vector<Microsoft::WRL::ComPtr<IMFSample>> input_samples_;
  std::size_t input_index_ = 0;
  // 输出样本池：MFT 不提供样本时复用，避免每帧 MFCreateSample/MFCreateMemoryBuffer。
  std::vector<Microsoft::WRL::ComPtr<IMFSample>> output_samples_;
  std::size_t output_index_ = 0;

  std::vector<uint8_t> nv12_;
  std::vector<uint8_t> sequence_header_;
  // 输入帧与输出帧错开一帧（NVENC 有约 1 帧流水延迟），按序回填采集时间戳。
  std::deque<uint64_t> pending_capture_ts_;

  bool async_ = false;
  bool hardware_ = false;
  bool texture_input_ = false;
  bool has_input_token_ = false;
  bool has_output_token_ = false;
  bool mft_provides_samples_ = false;
  bool inband_sps_ = false;           // 编码器是否把 SPS/PPS 内联到关键帧里（首帧判定后缓存）
  bool com_initialized_ = false;
  bool mf_started_ = false;

  uint32_t output_buffer_size_ = 0;
  int width_ = 0;
  int height_ = 0;
  int fps_ = 30;
  uint32_t bitrate_ = 0;
  int64_t frame_index_ = 0;
  uint64_t conv_us_ = 0;
  uint64_t input_us_ = 0;
  uint64_t output_us_ = 0;
};

}  // namespace rc

#endif  // _WIN32
