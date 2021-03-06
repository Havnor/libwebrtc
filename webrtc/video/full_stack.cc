/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include <stdio.h>

#include <deque>
#include <map>

#include "testing/gtest/include/gtest/gtest.h"

#include "webrtc/base/scoped_ptr.h"
#include "webrtc/base/thread_annotations.h"
#include "webrtc/call.h"
#include "webrtc/common_video/libyuv/include/webrtc_libyuv.h"
#include "webrtc/frame_callback.h"
#include "webrtc/modules/rtp_rtcp/interface/rtp_header_parser.h"
#include "webrtc/system_wrappers/interface/clock.h"
#include "webrtc/system_wrappers/interface/cpu_info.h"
#include "webrtc/system_wrappers/interface/critical_section_wrapper.h"
#include "webrtc/system_wrappers/interface/event_wrapper.h"
#include "webrtc/system_wrappers/interface/sleep.h"
#include "webrtc/test/call_test.h"
#include "webrtc/test/direct_transport.h"
#include "webrtc/test/encoder_settings.h"
#include "webrtc/test/fake_encoder.h"
#include "webrtc/test/frame_generator.h"
#include "webrtc/test/frame_generator_capturer.h"
#include "webrtc/test/statistics.h"
#include "webrtc/test/testsupport/fileutils.h"
#include "webrtc/typedefs.h"

namespace webrtc {

static const int kFullStackTestDurationSecs = 60;
static const int kSendStatsPollingIntervalMs = 1000;

enum class ContentMode {
  kRealTimeVideo,
  kScreensharingStaticImage,
  kScreensharingScrollingImage,
};

struct FullStackTestParams {
  const char* test_label;
  struct {
    const char* name;
    size_t width, height;
    int fps;
  } clip;
  ContentMode mode;
  int min_bitrate_bps;
  int target_bitrate_bps;
  int max_bitrate_bps;
  double avg_psnr_threshold;
  double avg_ssim_threshold;
  int test_durations_secs;
  std::string codec;
  FakeNetworkPipe::Config link;
};

class FullStackTest : public test::CallTest {
 protected:
  void RunTest(const FullStackTestParams& params);
};

class VideoAnalyzer : public PacketReceiver,
                      public newapi::Transport,
                      public VideoRenderer,
                      public VideoCaptureInput,
                      public EncodedFrameObserver {
 public:
  VideoAnalyzer(VideoCaptureInput* input,
                Transport* transport,
                const char* test_label,
                double avg_psnr_threshold,
                double avg_ssim_threshold,
                int duration_frames)
      : input_(input),
        transport_(transport),
        receiver_(nullptr),
        send_stream_(nullptr),
        test_label_(test_label),
        frames_to_process_(duration_frames),
        frames_recorded_(0),
        frames_processed_(0),
        dropped_frames_(0),
        last_render_time_(0),
        rtp_timestamp_delta_(0),
        avg_psnr_threshold_(avg_psnr_threshold),
        avg_ssim_threshold_(avg_ssim_threshold),
        comparison_available_event_(EventWrapper::Create()),
        done_(EventWrapper::Create()) {
    // Create thread pool for CPU-expensive PSNR/SSIM calculations.

    // Try to use about as many threads as cores, but leave kMinCoresLeft alone,
    // so that we don't accidentally starve "real" worker threads (codec etc).
    // Also, don't allocate more than kMaxComparisonThreads, even if there are
    // spare cores.

    uint32_t num_cores = CpuInfo::DetectNumberOfCores();
    DCHECK_GE(num_cores, 1u);
    static const uint32_t kMinCoresLeft = 4;
    static const uint32_t kMaxComparisonThreads = 8;

    if (num_cores <= kMinCoresLeft) {
      num_cores = 1;
    } else {
      num_cores -= kMinCoresLeft;
      num_cores = std::min(num_cores, kMaxComparisonThreads);
    }

    for (uint32_t i = 0; i < num_cores; ++i) {
      rtc::scoped_ptr<ThreadWrapper> thread =
          ThreadWrapper::CreateThread(&FrameComparisonThread, this, "Analyzer");
      EXPECT_TRUE(thread->Start());
      comparison_thread_pool_.push_back(thread.release());
    }

    stats_polling_thread_ =
        ThreadWrapper::CreateThread(&PollStatsThread, this, "StatsPoller");
    EXPECT_TRUE(stats_polling_thread_->Start());
  }

  ~VideoAnalyzer() {
    for (ThreadWrapper* thread : comparison_thread_pool_) {
      EXPECT_TRUE(thread->Stop());
      delete thread;
    }
  }

  virtual void SetReceiver(PacketReceiver* receiver) { receiver_ = receiver; }

  DeliveryStatus DeliverPacket(MediaType media_type, const uint8_t* packet,
                               size_t length) override {
    rtc::scoped_ptr<RtpHeaderParser> parser(RtpHeaderParser::Create());
    RTPHeader header;
    parser->Parse(packet, length, &header);
    {
      rtc::CritScope lock(&crit_);
      recv_times_[header.timestamp - rtp_timestamp_delta_] =
          Clock::GetRealTimeClock()->CurrentNtpInMilliseconds();
    }

    return receiver_->DeliverPacket(media_type, packet, length);
  }

  void IncomingCapturedFrame(const VideoFrame& video_frame) override {
    VideoFrame copy = video_frame;
    copy.set_timestamp(copy.ntp_time_ms() * 90);

    {
      rtc::CritScope lock(&crit_);
      if (first_send_frame_.IsZeroSize() && rtp_timestamp_delta_ == 0)
        first_send_frame_ = copy;

      frames_.push_back(copy);
    }

    input_->IncomingCapturedFrame(video_frame);
  }

  bool SendRtp(const uint8_t* packet, size_t length) override {
    rtc::scoped_ptr<RtpHeaderParser> parser(RtpHeaderParser::Create());
    RTPHeader header;
    parser->Parse(packet, length, &header);

    {
      rtc::CritScope lock(&crit_);
      if (rtp_timestamp_delta_ == 0) {
        rtp_timestamp_delta_ =
            header.timestamp - first_send_frame_.timestamp();
        first_send_frame_.Reset();
      }
      send_times_[header.timestamp - rtp_timestamp_delta_] =
          Clock::GetRealTimeClock()->CurrentNtpInMilliseconds();
    }

    return transport_->SendRtp(packet, length);
  }

  bool SendRtcp(const uint8_t* packet, size_t length) override {
    return transport_->SendRtcp(packet, length);
  }

  void EncodedFrameCallback(const EncodedFrame& frame) override {
    rtc::CritScope lock(&comparison_lock_);
    if (frames_recorded_ < frames_to_process_)
      encoded_frame_size_.AddSample(frame.length_);
  }

  void RenderFrame(const VideoFrame& video_frame,
                   int time_to_render_ms) override {
    int64_t render_time_ms =
        Clock::GetRealTimeClock()->CurrentNtpInMilliseconds();
    uint32_t send_timestamp = video_frame.timestamp() - rtp_timestamp_delta_;

    rtc::CritScope lock(&crit_);

    while (frames_.front().timestamp() < send_timestamp) {
      AddFrameComparison(frames_.front(), last_rendered_frame_, true,
                         render_time_ms);
      frames_.pop_front();
    }

    VideoFrame reference_frame = frames_.front();
    frames_.pop_front();
    assert(!reference_frame.IsZeroSize());
    EXPECT_EQ(reference_frame.timestamp(), send_timestamp);
    assert(reference_frame.timestamp() == send_timestamp);

    AddFrameComparison(reference_frame, video_frame, false, render_time_ms);

    last_rendered_frame_ = video_frame;
  }

  bool IsTextureSupported() const override { return false; }

  void Wait() {
    // Frame comparisons can be very expensive. Wait for test to be done, but
    // at time-out check if frames_processed is going up. If so, give it more
    // time, otherwise fail. Hopefully this will reduce test flakiness.

    int last_frames_processed = -1;
    EventTypeWrapper eventType;
    int iteration = 0;
    while ((eventType = done_->Wait(FullStackTest::kDefaultTimeoutMs)) !=
           kEventSignaled) {
      int frames_processed;
      {
        rtc::CritScope crit(&comparison_lock_);
        frames_processed = frames_processed_;
      }

      // Print some output so test infrastructure won't think we've crashed.
      const char* kKeepAliveMessages[3] = {
          "Uh, I'm-I'm not quite dead, sir.",
          "Uh, I-I think uh, I could pull through, sir.",
          "Actually, I think I'm all right to come with you--"};
      printf("- %s\n", kKeepAliveMessages[iteration++ % 3]);

      if (last_frames_processed == -1) {
        last_frames_processed = frames_processed;
        continue;
      }
      ASSERT_GT(frames_processed, last_frames_processed)
          << "Analyzer stalled while waiting for test to finish.";
      last_frames_processed = frames_processed;
    }

    if (iteration > 0)
      printf("- Farewell, sweet Concorde!\n");

    // Signal stats polling thread if that is still waiting and stop it now,
    // since it uses the send_stream_ reference that might be reclaimed after
    // returning from this method.
    done_->Set();
    EXPECT_TRUE(stats_polling_thread_->Stop());
  }

  VideoCaptureInput* input_;
  Transport* transport_;
  PacketReceiver* receiver_;
  VideoSendStream* send_stream_;

 private:
  struct FrameComparison {
    FrameComparison()
        : dropped(false), send_time_ms(0), recv_time_ms(0), render_time_ms(0) {}

    FrameComparison(const VideoFrame& reference,
                    const VideoFrame& render,
                    bool dropped,
                    int64_t send_time_ms,
                    int64_t recv_time_ms,
                    int64_t render_time_ms)
        : reference(reference),
          render(render),
          dropped(dropped),
          send_time_ms(send_time_ms),
          recv_time_ms(recv_time_ms),
          render_time_ms(render_time_ms) {}

    VideoFrame reference;
    VideoFrame render;
    bool dropped;
    int64_t send_time_ms;
    int64_t recv_time_ms;
    int64_t render_time_ms;
  };

  void AddFrameComparison(const VideoFrame& reference,
                          const VideoFrame& render,
                          bool dropped,
                          int64_t render_time_ms)
      EXCLUSIVE_LOCKS_REQUIRED(crit_) {
    int64_t send_time_ms = send_times_[reference.timestamp()];
    send_times_.erase(reference.timestamp());
    int64_t recv_time_ms = recv_times_[reference.timestamp()];
    recv_times_.erase(reference.timestamp());

    rtc::CritScope crit(&comparison_lock_);
    comparisons_.push_back(FrameComparison(reference, render, dropped,
                                           send_time_ms, recv_time_ms,
                                           render_time_ms));
    comparison_available_event_->Set();
  }

  static bool PollStatsThread(void* obj) {
    return static_cast<VideoAnalyzer*>(obj)->PollStats();
  }

  bool PollStats() {
    switch (done_->Wait(kSendStatsPollingIntervalMs)) {
      case kEventSignaled:
      case kEventError:
        done_->Set();  // Make sure main thread is also signaled.
        return false;
      case kEventTimeout:
        break;
      default:
        RTC_NOTREACHED();
    }

    VideoSendStream::Stats stats = send_stream_->GetStats();

    rtc::CritScope crit(&comparison_lock_);
    encode_frame_rate_.AddSample(stats.encode_frame_rate);
    encode_time_ms.AddSample(stats.avg_encode_time_ms);
    encode_usage_percent.AddSample(stats.encode_usage_percent);
    media_bitrate_bps.AddSample(stats.media_bitrate_bps);

    return true;
  }

  static bool FrameComparisonThread(void* obj) {
    return static_cast<VideoAnalyzer*>(obj)->CompareFrames();
  }

  bool CompareFrames() {
    if (AllFramesRecorded())
      return false;

    VideoFrame reference;
    VideoFrame render;
    FrameComparison comparison;

    if (!PopComparison(&comparison)) {
      // Wait until new comparison task is available, or test is done.
      // If done, wake up remaining threads waiting.
      comparison_available_event_->Wait(1000);
      if (AllFramesRecorded()) {
        comparison_available_event_->Set();
        return false;
      }
      return true;  // Try again.
    }

    PerformFrameComparison(comparison);

    if (FrameProcessed()) {
      PrintResults();
      done_->Set();
      comparison_available_event_->Set();
      return false;
    }

    return true;
  }

  bool PopComparison(FrameComparison* comparison) {
    rtc::CritScope crit(&comparison_lock_);
    // If AllFramesRecorded() is true, it means we have already popped
    // frames_to_process_ frames from comparisons_, so there is no more work
    // for this thread to be done. frames_processed_ might still be lower if
    // all comparisons are not done, but those frames are currently being
    // worked on by other threads.
    if (comparisons_.empty() || AllFramesRecorded())
      return false;

    *comparison = comparisons_.front();
    comparisons_.pop_front();

    FrameRecorded();
    return true;
  }

  // Increment counter for number of frames received for comparison.
  void FrameRecorded() {
    rtc::CritScope crit(&comparison_lock_);
    ++frames_recorded_;
  }

  // Returns true if all frames to be compared have been taken from the queue.
  bool AllFramesRecorded() {
    rtc::CritScope crit(&comparison_lock_);
    assert(frames_recorded_ <= frames_to_process_);
    return frames_recorded_ == frames_to_process_;
  }

  // Increase count of number of frames processed. Returns true if this was the
  // last frame to be processed.
  bool FrameProcessed() {
    rtc::CritScope crit(&comparison_lock_);
    ++frames_processed_;
    assert(frames_processed_ <= frames_to_process_);
    return frames_processed_ == frames_to_process_;
  }

  void PrintResults() {
    rtc::CritScope crit(&comparison_lock_);
    PrintResult("psnr", psnr_, " dB");
    PrintResult("ssim", ssim_, "");
    PrintResult("sender_time", sender_time_, " ms");
    printf("RESULT dropped_frames: %s = %d frames\n", test_label_,
           dropped_frames_);
    PrintResult("receiver_time", receiver_time_, " ms");
    PrintResult("total_delay_incl_network", end_to_end_, " ms");
    PrintResult("time_between_rendered_frames", rendered_delta_, " ms");
    PrintResult("encoded_frame_size", encoded_frame_size_, " bytes");
    PrintResult("encode_frame_rate", encode_frame_rate_, " fps");
    PrintResult("encode_time", encode_time_ms, " ms");
    PrintResult("encode_usage_percent", encode_usage_percent, " percent");
    PrintResult("media_bitrate", media_bitrate_bps, " bps");

    EXPECT_GT(psnr_.Mean(), avg_psnr_threshold_);
    EXPECT_GT(ssim_.Mean(), avg_ssim_threshold_);
  }

  void PerformFrameComparison(const FrameComparison& comparison) {
    // Perform expensive psnr and ssim calculations while not holding lock.
    double psnr = I420PSNR(&comparison.reference, &comparison.render);
    double ssim = I420SSIM(&comparison.reference, &comparison.render);

    rtc::CritScope crit(&comparison_lock_);
    psnr_.AddSample(psnr);
    ssim_.AddSample(ssim);
    if (comparison.dropped) {
      ++dropped_frames_;
      return;
    }
    if (last_render_time_ != 0)
      rendered_delta_.AddSample(comparison.render_time_ms - last_render_time_);
    last_render_time_ = comparison.render_time_ms;

    int64_t input_time_ms = comparison.reference.ntp_time_ms();
    sender_time_.AddSample(comparison.send_time_ms - input_time_ms);
    receiver_time_.AddSample(comparison.render_time_ms -
                             comparison.recv_time_ms);
    end_to_end_.AddSample(comparison.render_time_ms - input_time_ms);
  }

  void PrintResult(const char* result_type,
                   test::Statistics stats,
                   const char* unit) {
    printf("RESULT %s: %s = {%f, %f}%s\n",
           result_type,
           test_label_,
           stats.Mean(),
           stats.StandardDeviation(),
           unit);
  }

  const char* const test_label_;
  test::Statistics sender_time_ GUARDED_BY(comparison_lock_);
  test::Statistics receiver_time_ GUARDED_BY(comparison_lock_);
  test::Statistics psnr_ GUARDED_BY(comparison_lock_);
  test::Statistics ssim_ GUARDED_BY(comparison_lock_);
  test::Statistics end_to_end_ GUARDED_BY(comparison_lock_);
  test::Statistics rendered_delta_ GUARDED_BY(comparison_lock_);
  test::Statistics encoded_frame_size_ GUARDED_BY(comparison_lock_);
  test::Statistics encode_frame_rate_ GUARDED_BY(comparison_lock_);
  test::Statistics encode_time_ms GUARDED_BY(comparison_lock_);
  test::Statistics encode_usage_percent GUARDED_BY(comparison_lock_);
  test::Statistics media_bitrate_bps GUARDED_BY(comparison_lock_);

  const int frames_to_process_;
  int frames_recorded_;
  int frames_processed_;
  int dropped_frames_;
  int64_t last_render_time_;
  uint32_t rtp_timestamp_delta_;

  rtc::CriticalSection crit_;
  std::deque<VideoFrame> frames_ GUARDED_BY(crit_);
  VideoFrame last_rendered_frame_ GUARDED_BY(crit_);
  std::map<uint32_t, int64_t> send_times_ GUARDED_BY(crit_);
  std::map<uint32_t, int64_t> recv_times_ GUARDED_BY(crit_);
  VideoFrame first_send_frame_ GUARDED_BY(crit_);
  const double avg_psnr_threshold_;
  const double avg_ssim_threshold_;

  rtc::CriticalSection comparison_lock_;
  std::vector<ThreadWrapper*> comparison_thread_pool_;
  rtc::scoped_ptr<ThreadWrapper> stats_polling_thread_;
  const rtc::scoped_ptr<EventWrapper> comparison_available_event_;
  std::deque<FrameComparison> comparisons_ GUARDED_BY(comparison_lock_);
  const rtc::scoped_ptr<EventWrapper> done_;
};

void FullStackTest::RunTest(const FullStackTestParams& params) {
  test::DirectTransport send_transport(params.link);
  test::DirectTransport recv_transport(params.link);
  VideoAnalyzer analyzer(nullptr, &send_transport, params.test_label,
                         params.avg_psnr_threshold, params.avg_ssim_threshold,
                         params.test_durations_secs * params.clip.fps);

  CreateCalls(Call::Config(), Call::Config());

  analyzer.SetReceiver(receiver_call_->Receiver());
  send_transport.SetReceiver(&analyzer);
  recv_transport.SetReceiver(sender_call_->Receiver());

  CreateSendConfig(1, &analyzer);

  rtc::scoped_ptr<VideoEncoder> encoder;
  if (params.codec == "VP8") {
    encoder =
        rtc::scoped_ptr<VideoEncoder>(VideoEncoder::Create(VideoEncoder::kVp8));
    send_config_.encoder_settings.encoder = encoder.get();
    send_config_.encoder_settings.payload_name = "VP8";
  } else if (params.codec == "VP9") {
    encoder =
        rtc::scoped_ptr<VideoEncoder>(VideoEncoder::Create(VideoEncoder::kVp9));
    send_config_.encoder_settings.encoder = encoder.get();
    send_config_.encoder_settings.payload_name = "VP9";
  } else {
    RTC_NOTREACHED() << "Codec not supported!";
    return;
  }
  send_config_.encoder_settings.payload_type = 124;

  send_config_.rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
  send_config_.rtp.rtx.ssrcs.push_back(kSendRtxSsrcs[0]);
  send_config_.rtp.rtx.payload_type = kSendRtxPayloadType;

  VideoStream* stream = &encoder_config_.streams[0];
  stream->width = params.clip.width;
  stream->height = params.clip.height;
  stream->min_bitrate_bps = params.min_bitrate_bps;
  stream->target_bitrate_bps = params.target_bitrate_bps;
  stream->max_bitrate_bps = params.max_bitrate_bps;
  stream->max_framerate = params.clip.fps;

  VideoCodecVP8 vp8_settings;
  VideoCodecVP9 vp9_settings;
  if (params.mode == ContentMode::kScreensharingStaticImage ||
      params.mode == ContentMode::kScreensharingScrollingImage) {
    encoder_config_.content_type = VideoEncoderConfig::ContentType::kScreen;
    encoder_config_.min_transmit_bitrate_bps = 400 * 1000;
    if (params.codec == "VP8") {
      vp8_settings = VideoEncoder::GetDefaultVp8Settings();
      vp8_settings.denoisingOn = false;
      vp8_settings.frameDroppingOn = false;
      vp8_settings.numberOfTemporalLayers = 2;
      encoder_config_.encoder_specific_settings = &vp8_settings;
    } else if (params.codec == "VP9") {
      vp9_settings = VideoEncoder::GetDefaultVp9Settings();
      vp9_settings.denoisingOn = false;
      vp9_settings.frameDroppingOn = false;
      vp9_settings.numberOfTemporalLayers = 2;
      encoder_config_.encoder_specific_settings = &vp9_settings;
    }

    stream->temporal_layer_thresholds_bps.clear();
    stream->temporal_layer_thresholds_bps.push_back(stream->target_bitrate_bps);
  }

  CreateMatchingReceiveConfigs(&recv_transport);
  receive_configs_[0].renderer = &analyzer;
  receive_configs_[0].rtp.nack.rtp_history_ms = kNackRtpHistoryMs;
  receive_configs_[0].rtp.rtx[kSendRtxPayloadType].ssrc = kSendRtxSsrcs[0];
  receive_configs_[0].rtp.rtx[kSendRtxPayloadType].payload_type =
      kSendRtxPayloadType;

  for (auto& config : receive_configs_)
    config.pre_decode_callback = &analyzer;
  CreateStreams();
  analyzer.input_ = send_stream_->Input();
  analyzer.send_stream_ = send_stream_;

  std::vector<std::string> slides;
  slides.push_back(test::ResourcePath("web_screenshot_1850_1110", "yuv"));
  slides.push_back(test::ResourcePath("presentation_1850_1110", "yuv"));
  slides.push_back(test::ResourcePath("photo_1850_1110", "yuv"));
  slides.push_back(test::ResourcePath("difficult_photo_1850_1110", "yuv"));
  size_t kSlidesWidth = 1850;
  size_t kSlidesHeight = 1110;

  Clock* clock = Clock::GetRealTimeClock();
  rtc::scoped_ptr<test::FrameGenerator> frame_generator;

  switch (params.mode) {
    case ContentMode::kRealTimeVideo:
      frame_generator.reset(test::FrameGenerator::CreateFromYuvFile(
          std::vector<std::string>(1,
                                   test::ResourcePath(params.clip.name, "yuv")),
          params.clip.width, params.clip.height, 1));
      break;
    case ContentMode::kScreensharingScrollingImage:
      frame_generator.reset(
          test::FrameGenerator::CreateScrollingInputFromYuvFiles(
              clock, slides, kSlidesWidth, kSlidesHeight, params.clip.width,
              params.clip.height, 2000,
              8000));  // Scroll for 2 seconds, then pause for 8.
      break;
    case ContentMode::kScreensharingStaticImage:
      frame_generator.reset(test::FrameGenerator::CreateFromYuvFile(
          slides, kSlidesWidth, kSlidesHeight,
          10 * params.clip.fps));  // Cycle image every 10 seconds.
      break;
  }

  ASSERT_TRUE(frame_generator.get() != nullptr);
  frame_generator_capturer_.reset(new test::FrameGeneratorCapturer(
      clock, &analyzer, frame_generator.release(), params.clip.fps));
  ASSERT_TRUE(frame_generator_capturer_->Init());

  Start();

  analyzer.Wait();

  send_transport.StopSending();
  recv_transport.StopSending();

  Stop();

  DestroyStreams();
}

TEST_F(FullStackTest, ParisQcifWithoutPacketLoss) {
  FullStackTestParams paris_qcif = {"net_delay_0_0_plr_0",
                                    {"paris_qcif", 176, 144, 30},
                                    ContentMode::kRealTimeVideo,
                                    300000,
                                    300000,
                                    300000,
                                    36.0,
                                    0.96,
                                    kFullStackTestDurationSecs,
                                    "VP8"};
  RunTest(paris_qcif);
}

TEST_F(FullStackTest, ForemanCifWithoutPacketLoss) {
  // TODO(pbos): Decide on psnr/ssim thresholds for foreman_cif.
  FullStackTestParams foreman_cif = {"foreman_cif_net_delay_0_0_plr_0",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     700000,
                                     700000,
                                     700000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  RunTest(foreman_cif);
}

TEST_F(FullStackTest, ForemanCifPlr5) {
  FullStackTestParams foreman_cif = {"foreman_cif_delay_50_0_plr_5",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     30000,
                                     500000,
                                     2000000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  foreman_cif.link.loss_percent = 5;
  foreman_cif.link.queue_delay_ms = 50;
  RunTest(foreman_cif);
}

TEST_F(FullStackTest, ForemanCif500kbps) {
  FullStackTestParams foreman_cif = {"foreman_cif_500kbps",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     30000,
                                     500000,
                                     2000000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  foreman_cif.link.queue_length_packets = 0;
  foreman_cif.link.queue_delay_ms = 0;
  foreman_cif.link.link_capacity_kbps = 500;
  RunTest(foreman_cif);
}

TEST_F(FullStackTest, ForemanCif500kbpsLimitedQueue) {
  FullStackTestParams foreman_cif = {"foreman_cif_500kbps_32pkts_queue",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     30000,
                                     500000,
                                     2000000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  foreman_cif.link.queue_length_packets = 32;
  foreman_cif.link.queue_delay_ms = 0;
  foreman_cif.link.link_capacity_kbps = 500;
  RunTest(foreman_cif);
}

TEST_F(FullStackTest, ForemanCif500kbps100ms) {
  FullStackTestParams foreman_cif = {"foreman_cif_500kbps_100ms",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     30000,
                                     500000,
                                     2000000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  foreman_cif.link.queue_length_packets = 0;
  foreman_cif.link.queue_delay_ms = 100;
  foreman_cif.link.link_capacity_kbps = 500;
  RunTest(foreman_cif);
}

TEST_F(FullStackTest, ForemanCif500kbps100msLimitedQueue) {
  FullStackTestParams foreman_cif = {"foreman_cif_500kbps_100ms_32pkts_queue",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     30000,
                                     500000,
                                     2000000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  foreman_cif.link.queue_length_packets = 32;
  foreman_cif.link.queue_delay_ms = 100;
  foreman_cif.link.link_capacity_kbps = 500;
  RunTest(foreman_cif);
}

TEST_F(FullStackTest, ForemanCif1000kbps100msLimitedQueue) {
  FullStackTestParams foreman_cif = {"foreman_cif_1000kbps_100ms_32pkts_queue",
                                     {"foreman_cif", 352, 288, 30},
                                     ContentMode::kRealTimeVideo,
                                     30000,
                                     2000000,
                                     2000000,
                                     0.0,
                                     0.0,
                                     kFullStackTestDurationSecs,
                                     "VP8"};
  foreman_cif.link.queue_length_packets = 32;
  foreman_cif.link.queue_delay_ms = 100;
  foreman_cif.link.link_capacity_kbps = 1000;
  RunTest(foreman_cif);
}

// Temporarily disabled on Android due to low test timeouts.
// https://code.google.com/p/chromium/issues/detail?id=513170
#include "webrtc/test/testsupport/gtest_disable.h"
TEST_F(FullStackTest, DISABLED_ON_ANDROID(ScreenshareSlidesVP8_2TL)) {
  FullStackTestParams screenshare_params = {
      "screenshare_slides",
      {"screenshare_slides", 1850, 1110, 5},
      ContentMode::kScreensharingStaticImage,
      50000,
      200000,
      2000000,
      0.0,
      0.0,
      kFullStackTestDurationSecs,
      "VP8"};
  RunTest(screenshare_params);
}

TEST_F(FullStackTest, DISABLED_ON_ANDROID(ScreenshareSlidesVP8_2TL_Scroll)) {
  FullStackTestParams screenshare_params = {
      "screenshare_slides_scrolling",
      // Crop height by two, scrolling vertically only.
      {"screenshare_slides_scrolling", 1850, 1110 / 2, 5},
      ContentMode::kScreensharingScrollingImage,
      50000,
      200000,
      2000000,
      0.0,
      0.0,
      kFullStackTestDurationSecs,
      "VP8"};
  RunTest(screenshare_params);
}

// Disabled on Android along with VP8 screenshare above.
TEST_F(FullStackTest, DISABLED_ON_ANDROID(ScreenshareSlidesVP9_2TL)) {
  FullStackTestParams screenshare_params = {
      "screenshare_slides_vp9_2tl",
      {"screenshare_slides", 1850, 1110, 5},
      ContentMode::kScreensharingStaticImage,
      50000,
      200000,
      2000000,
      0.0,
      0.0,
      kFullStackTestDurationSecs,
      "VP9"};
  RunTest(screenshare_params);
}
}  // namespace webrtc
