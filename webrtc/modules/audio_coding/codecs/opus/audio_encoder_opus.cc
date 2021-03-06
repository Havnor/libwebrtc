/*
 *  Copyright (c) 2014 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/modules/audio_coding/codecs/opus/interface/audio_encoder_opus.h"

#include "webrtc/base/checks.h"
#include "webrtc/common_types.h"
#include "webrtc/modules/audio_coding/codecs/opus/interface/opus_interface.h"

namespace webrtc {

namespace {

const int kMinBitrateBps = 500;
const int kMaxBitrateBps = 512000;

// TODO(tlegrand): Remove this code when we have proper APIs to set the
// complexity at a higher level.
#if defined(WEBRTC_ANDROID) || defined(WEBRTC_IOS) || defined(WEBRTC_ARCH_ARM)
// If we are on Android, iOS and/or ARM, use a lower complexity setting as
// default, to save encoder complexity.
const int kDefaultComplexity = 5;
#else
const int kDefaultComplexity = 9;
#endif

// We always encode at 48 kHz.
const int kSampleRateHz = 48000;

}  // namespace

AudioEncoderOpus::Config::Config()
    : frame_size_ms(20),
      num_channels(1),
      payload_type(120),
      application(kVoip),
      bitrate_bps(64000),
      fec_enabled(false),
      max_playback_rate_hz(48000),
      complexity(kDefaultComplexity),
      dtx_enabled(false) {
}

bool AudioEncoderOpus::Config::IsOk() const {
  if (frame_size_ms <= 0 || frame_size_ms % 10 != 0)
    return false;
  if (num_channels != 1 && num_channels != 2)
    return false;
  if (bitrate_bps < kMinBitrateBps || bitrate_bps > kMaxBitrateBps)
    return false;
  if (complexity < 0 || complexity > 10)
    return false;
  return true;
}

AudioEncoderOpus::AudioEncoderOpus(const Config& config)
    : num_10ms_frames_per_packet_(
          static_cast<size_t>(rtc::CheckedDivExact(config.frame_size_ms, 10))),
      num_channels_(config.num_channels),
      payload_type_(config.payload_type),
      application_(config.application),
      dtx_enabled_(config.dtx_enabled),
      samples_per_10ms_frame_(static_cast<size_t>(
          rtc::CheckedDivExact(kSampleRateHz, 100) * num_channels_)),
      packet_loss_rate_(0.0) {
  CHECK(config.IsOk());
  input_buffer_.reserve(num_10ms_frames_per_packet_ * samples_per_10ms_frame_);
  CHECK_EQ(0, WebRtcOpus_EncoderCreate(&inst_, num_channels_, application_));
  SetTargetBitrate(config.bitrate_bps);
  if (config.fec_enabled) {
    CHECK_EQ(0, WebRtcOpus_EnableFec(inst_));
  } else {
    CHECK_EQ(0, WebRtcOpus_DisableFec(inst_));
  }
  CHECK_EQ(0,
           WebRtcOpus_SetMaxPlaybackRate(inst_, config.max_playback_rate_hz));
  CHECK_EQ(0, WebRtcOpus_SetComplexity(inst_, config.complexity));
  if (config.dtx_enabled) {
    CHECK_EQ(0, WebRtcOpus_EnableDtx(inst_));
  } else {
    CHECK_EQ(0, WebRtcOpus_DisableDtx(inst_));
  }
}

AudioEncoderOpus::~AudioEncoderOpus() {
  CHECK_EQ(0, WebRtcOpus_EncoderFree(inst_));
}

int AudioEncoderOpus::SampleRateHz() const {
  return kSampleRateHz;
}

int AudioEncoderOpus::NumChannels() const {
  return num_channels_;
}

size_t AudioEncoderOpus::MaxEncodedBytes() const {
  // Calculate the number of bytes we expect the encoder to produce,
  // then multiply by two to give a wide margin for error.
  size_t bytes_per_millisecond =
      static_cast<size_t>(bitrate_bps_ / (1000 * 8) + 1);
  size_t approx_encoded_bytes =
      num_10ms_frames_per_packet_ * 10 * bytes_per_millisecond;
  return 2 * approx_encoded_bytes;
}

size_t AudioEncoderOpus::Num10MsFramesInNextPacket() const {
  return num_10ms_frames_per_packet_;
}

size_t AudioEncoderOpus::Max10MsFramesInAPacket() const {
  return num_10ms_frames_per_packet_;
}

int AudioEncoderOpus::GetTargetBitrate() const {
  return bitrate_bps_;
}

void AudioEncoderOpus::SetTargetBitrate(int bits_per_second) {
  bitrate_bps_ = std::max(std::min(bits_per_second, kMaxBitrateBps),
                          kMinBitrateBps);
  CHECK_EQ(WebRtcOpus_SetBitRate(inst_, bitrate_bps_), 0);
}

void AudioEncoderOpus::SetProjectedPacketLossRate(double fraction) {
  DCHECK_GE(fraction, 0.0);
  DCHECK_LE(fraction, 1.0);
  // Optimize the loss rate to configure Opus. Basically, optimized loss rate is
  // the input loss rate rounded down to various levels, because a robustly good
  // audio quality is achieved by lowering the packet loss down.
  // Additionally, to prevent toggling, margins are used, i.e., when jumping to
  // a loss rate from below, a higher threshold is used than jumping to the same
  // level from above.
  const double kPacketLossRate20 = 0.20;
  const double kPacketLossRate10 = 0.10;
  const double kPacketLossRate5 = 0.05;
  const double kPacketLossRate1 = 0.01;
  const double kLossRate20Margin = 0.02;
  const double kLossRate10Margin = 0.01;
  const double kLossRate5Margin = 0.01;
  double opt_loss_rate;
  if (fraction >=
      kPacketLossRate20 +
          kLossRate20Margin *
              (kPacketLossRate20 - packet_loss_rate_ > 0 ? 1 : -1)) {
    opt_loss_rate = kPacketLossRate20;
  } else if (fraction >=
             kPacketLossRate10 +
                 kLossRate10Margin *
                     (kPacketLossRate10 - packet_loss_rate_ > 0 ? 1 : -1)) {
    opt_loss_rate = kPacketLossRate10;
  } else if (fraction >=
             kPacketLossRate5 +
                 kLossRate5Margin *
                     (kPacketLossRate5 - packet_loss_rate_ > 0 ? 1 : -1)) {
    opt_loss_rate = kPacketLossRate5;
  } else if (fraction >= kPacketLossRate1) {
    opt_loss_rate = kPacketLossRate1;
  } else {
    opt_loss_rate = 0;
  }

  if (packet_loss_rate_ != opt_loss_rate) {
    // Ask the encoder to change the target packet loss rate.
    CHECK_EQ(WebRtcOpus_SetPacketLossRate(
                 inst_, static_cast<int32_t>(opt_loss_rate * 100 + .5)),
             0);
    packet_loss_rate_ = opt_loss_rate;
  }
}

AudioEncoder::EncodedInfo AudioEncoderOpus::EncodeInternal(
    uint32_t rtp_timestamp,
    const int16_t* audio,
    size_t max_encoded_bytes,
    uint8_t* encoded) {
  if (input_buffer_.empty())
    first_timestamp_in_buffer_ = rtp_timestamp;
  input_buffer_.insert(input_buffer_.end(), audio,
                       audio + samples_per_10ms_frame_);
  if (input_buffer_.size() <
      (num_10ms_frames_per_packet_ * samples_per_10ms_frame_)) {
    return EncodedInfo();
  }
  CHECK_EQ(input_buffer_.size(),
           num_10ms_frames_per_packet_ * samples_per_10ms_frame_);
  int status = WebRtcOpus_Encode(
      inst_, &input_buffer_[0],
      rtc::CheckedDivExact(input_buffer_.size(),
                           static_cast<size_t>(num_channels_)),
      max_encoded_bytes, encoded);
  CHECK_GE(status, 0);  // Fails only if fed invalid data.
  input_buffer_.clear();
  EncodedInfo info;
  info.encoded_bytes = static_cast<size_t>(status);
  info.encoded_timestamp = first_timestamp_in_buffer_;
  info.payload_type = payload_type_;
  info.send_even_if_empty = true;  // Allows Opus to send empty packets.
  info.speech = (status > 0);
  return info;
}

namespace {
AudioEncoderOpus::Config CreateConfig(const CodecInst& codec_inst) {
  AudioEncoderOpus::Config config;
  config.frame_size_ms = rtc::CheckedDivExact(codec_inst.pacsize, 48);
  config.num_channels = codec_inst.channels;
  config.bitrate_bps = codec_inst.rate;
  config.payload_type = codec_inst.pltype;
  config.application = (config.num_channels == 1 ? AudioEncoderOpus::kVoip
                                                 : AudioEncoderOpus::kAudio);
  return config;
}
}  // namespace

AudioEncoderMutableOpus::AudioEncoderMutableOpus(const CodecInst& codec_inst)
    : AudioEncoderMutableImpl<AudioEncoderOpus>(CreateConfig(codec_inst)) {
}

bool AudioEncoderMutableOpus::SetFec(bool enable) {
  auto conf = config();
  conf.fec_enabled = enable;
  return Reconstruct(conf);
}

bool AudioEncoderMutableOpus::SetDtx(bool enable) {
  auto conf = config();
  conf.dtx_enabled = enable;
  return Reconstruct(conf);
}

bool AudioEncoderMutableOpus::SetApplication(Application application) {
  auto conf = config();
  switch (application) {
    case kApplicationSpeech:
      conf.application = AudioEncoderOpus::kVoip;
      break;
    case kApplicationAudio:
      conf.application = AudioEncoderOpus::kAudio;
      break;
  }
  return Reconstruct(conf);
}

bool AudioEncoderMutableOpus::SetMaxPlaybackRate(int frequency_hz) {
  auto conf = config();
  conf.max_playback_rate_hz = frequency_hz;
  return Reconstruct(conf);
}

}  // namespace webrtc
