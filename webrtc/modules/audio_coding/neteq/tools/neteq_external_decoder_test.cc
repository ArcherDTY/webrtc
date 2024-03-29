/*
 *  Copyright (c) 2015 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include "webrtc/modules/audio_coding/neteq/tools/neteq_external_decoder_test.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "webrtc/base/format_macros.h"

namespace webrtc {
namespace test {

NetEqExternalDecoderTest::NetEqExternalDecoderTest(NetEqDecoder codec,
                                                   AudioDecoder* decoder)
    : codec_(codec),
      decoder_(decoder),
      sample_rate_hz_(CodecSampleRateHz(codec_)),
      channels_(decoder_->Channels()) {
  NetEq::Config config;
  config.sample_rate_hz = sample_rate_hz_;
  neteq_.reset(NetEq::Create(config));
  printf("%" PRIuS "\n", channels_);
}

void NetEqExternalDecoderTest::Init() {
  ASSERT_EQ(NetEq::kOK,
            neteq_->RegisterExternalDecoder(decoder_, codec_, name_,
                                            kPayloadType, sample_rate_hz_));
}

void NetEqExternalDecoderTest::InsertPacket(
    WebRtcRTPHeader rtp_header,
    rtc::ArrayView<const uint8_t> payload,
    uint32_t receive_timestamp) {
  ASSERT_EQ(NetEq::kOK,
            neteq_->InsertPacket(rtp_header, payload, receive_timestamp));
}

void NetEqExternalDecoderTest::GetOutputAudio(AudioFrame* output,
                                              NetEqOutputType* output_type) {
  // Get audio from regular instance.
  EXPECT_EQ(NetEq::kOK, neteq_->GetAudio(output, output_type));
  EXPECT_EQ(channels_, output->num_channels_);
  EXPECT_EQ(static_cast<size_t>(kOutputLengthMs * sample_rate_hz_ / 1000),
            output->samples_per_channel_);
  EXPECT_EQ(sample_rate_hz_, neteq_->last_output_sample_rate_hz());
}

}  // namespace test
}  // namespace webrtc
