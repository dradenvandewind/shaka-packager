// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_
#define PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_

#include <cstdint>
#include <vector>

#include <packager/macros/classes.h>
#include "packager/media/base/fourccs.h"
#include "packager/media/base/video_stream_info.h"
#include "packager/media/codecs/decoder_configuration_record.h"
#include <packager/media/codecs/h266_parser.h>

namespace shaka {
namespace media {


/// Classe représentant VvcDecoderConfigurationRecord (vvcC box)
/// Selon ISO/IEC 14496-15:2022 Section 8.3.3.1 et ITU-T H.266
class VvcDecoderConfigurationRecord : public DecoderConfigurationRecord {
 public:
  VvcDecoderConfigurationRecord();
  ~VvcDecoderConfigurationRecord() override;

  // Can specify an existing parser to use for referencing to previously parsed
  // parameter sets.
  void SetParser(H266Parser* parser) {
    parser_ = parser;
    internal_parser_used_ = false;
  }
  bool ParseVVCConfig(const std::vector<uint8_t>& data);

  /// @return The codec string.
  std::string GetCodecString(FourCC codec_fourcc) const;

 private:
  bool ParseInternal() override;

  // If this is set true, then L-HEVC configuration record is also parsed.
  bool layered_ = false;
  bool internal_parser_used_ = true;
  H266Parser* parser_ = nullptr;
  H266Parser internal_parser_;

  uint8_t version_ = 0;
  uint8_t general_profile_space_ = 0;
  bool general_tier_flag_ = false;
  uint8_t general_profile_idc_ = 0;
  uint32_t general_profile_compatibility_flags_ = 0;
  std::vector<uint8_t> general_constraint_indicator_flags_;
  uint8_t general_level_idc_ = 0;

};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_