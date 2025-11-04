// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_
#define PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_

#include <cstdint>
#include <vector>

#include "packager/media/base/fourccs.h"
#include "packager/media/base/video_stream_info.h"

namespace shaka {
namespace media {

class BufferReader;
class BufferWriter;

/// Structure representing VVC PTL (Profile, Tier, Level)
/// According to ISO/IEC 14496-15:2022 Section 8.3.3.1.2
struct VvcPTLRecord {
  uint8_t num_bytes_constraint_info = 0;
  uint8_t general_profile_idc = 0;
  uint8_t general_tier_flag = 0;
  uint8_t general_level_idc = 0;
  uint8_t ptl_frame_only_constraint_flag = 0;
  uint8_t ptl_multilayer_enabled_flag = 0;
  std::vector<uint8_t> general_constraint_info;  // Variable size
  std::vector<uint8_t> ptl_sublayer_level_present_flag;
  std::vector<uint8_t> sublayer_level_idc;
  uint8_t num_sub_profiles = 0;
  std::vector<uint32_t> general_sub_profile_idc;

  /// Parse PTL from buffer
  bool Parse(const uint8_t* data, size_t size, uint8_t max_sublayers_minus1);
  
  /// Write PTL to buffer
  void Write(BufferWriter* writer, uint8_t max_sublayers_minus1) const;
  
  /// Returns size in bytes
  size_t ComputeSize(uint8_t max_sublayers_minus1) const;
};

/// Structure for NAL unit array
struct VvcNalArray {
  uint8_t array_completeness = 0;  // 1 bit
  uint8_t nal_unit_type = 0;       // 5 bits
  std::vector<std::vector<uint8_t>> nal_units;
};

/// Class representing VvcDecoderConfigurationRecord (vvcC box)
/// According to ISO/IEC 14496-15:2022 Section 8.3.3.1
class VvcDecoderConfigurationRecord {
 public:
  VvcDecoderConfigurationRecord();
  ~VvcDecoderConfigurationRecord();

  /// Parse from buffer (vvcC format)
  bool Parse(const uint8_t* data, size_t size);
  
  /// Parse from raw NAL units (Annex B format)
  bool ParseFromNalUnits(const std::vector<uint8_t>& nal_units);
  
  /// Write configuration to buffer
  void Write(BufferWriter* writer) const;
  
  /// Returns total size in bytes
  size_t ComputeSize() const;
  
  /// Extracts codec string for DASH/HLS (ex: "vvc1.1.L93.0")
  std::string GetCodecString(FourCC codec_fourcc) const;
  
  /// Gets video dimensions from SPS
  bool GetVideoDimensions(uint32_t* width, uint32_t* height) const;
  
  /// Gets chroma format
  VideoStreamInfo::ChromaSubsampling GetChromaSubsampling() const;
  
  /// Accessors
  uint8_t length_size_minus_one() const { return length_size_minus_one_; }
  void set_length_size_minus_one(uint8_t value) { length_size_minus_one_ = value; }
  
  bool ptl_present_flag() const { return ptl_present_flag_; }
  const VvcPTLRecord& ptl_record() const { return ptl_record_; }
  
  uint16_t max_picture_width() const { return max_picture_width_; }
  uint16_t max_picture_height() const { return max_picture_height_; }
  
  uint16_t avg_frame_rate() const { return avg_frame_rate_; }
  void set_avg_frame_rate(uint16_t rate) { avg_frame_rate_ = rate; }
  
  const std::vector<VvcNalArray>& arrays() const { return arrays_; }
  
  /// Add NAL unit to configuration
  void AddNalUnit(uint8_t nal_unit_type, 
                  const uint8_t* data, 
                  size_t size,
                  bool array_completeness = false);

 private:
  // Parse parameter sets to extract information
  bool ParseParameterSets();
  
  // Parse VVC SPS
  bool ParseSPS(const uint8_t* data, size_t size);
  
  // Parse VVC VPS
  bool ParseVPS(const uint8_t* data, size_t size);

  // Configuration fields according to ISO/IEC 14496-15:2022
  uint8_t length_size_minus_one_ = 3;  // Typically 3 (4 bytes)
  bool ptl_present_flag_ = true;
  
  // If ptl_present_flag == 1
  uint16_t ols_idx_ = 0;
  uint8_t num_sublayers_ = 0;
  uint8_t constant_frame_rate_ = 0;
  uint8_t chroma_format_idc_ = 1;  // 4:2:0 by default
  uint8_t bit_depth_minus8_ = 0;   // 8 bits by default
  
  VvcPTLRecord ptl_record_;
  
  // Picture dimensions
  uint16_t max_picture_width_ = 0;
  uint16_t max_picture_height_ = 0;
  uint16_t avg_frame_rate_ = 0;
  
  // NAL unit arrays
  std::vector<VvcNalArray> arrays_;
  
  // Raw parameter sets data for parsing
  std::vector<uint8_t> vps_data_;
  std::vector<uint8_t> sps_data_;
  std::vector<uint8_t> pps_data_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_