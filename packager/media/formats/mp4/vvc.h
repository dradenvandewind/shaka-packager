// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_FORMATS_MP4_VVC_H_
#define PACKAGER_MEDIA_FORMATS_MP4_VVC_H_

#include <vector>

#include "packager/media/formats/mp4/box.h"
#include "packager/media/formats/mp4/box_definitions.h"

namespace shaka {
namespace media {
namespace mp4 {

/// VVC configuration box (vvcC)
/// Selon ISO/IEC 14496-15:2022 Section 8.3.3.1
class VVCDecoderConfigurationRecord : public Box {
 public:
  DECLARE_BOX_METHODS(VVCDecoderConfigurationRecord);

  /// Structure pour le PTL (Profile, Tier, Level)
  struct VvcPTLRecord {
    uint8_t ols_in_scope = 1;
    uint8_t num_sublayers = 0;
    uint8_t general_profile_idc = 0;
    uint8_t general_tier_flag = 0;
    uint8_t general_level_idc = 0;
    uint8_t ptl_frame_only_constraint_flag = 0;
    uint8_t ptl_multilayer_enabled_flag = 0;
    std::vector<uint8_t> general_constraint_info;
    std::vector<uint8_t> ptl_sublayer_level_present_flag;
    std::vector<uint8_t> sublayer_level_idc;
    uint8_t num_sub_profiles = 0;
    std::vector<uint32_t> general_sub_profile_idc;
  };

  /// Structure pour un tableau de NAL units
  struct VvcNalArray {
    uint8_t array_completeness = 0;
    uint8_t nal_unit_type = 0;
    std::vector<std::vector<uint8_t>> nal_units;
  };

  // Accesseurs
  uint8_t length_size_minus_one() const { return length_size_minus_one_; }
  void set_length_size_minus_one(uint8_t value) { length_size_minus_one_ = value; }

  bool ptl_present_flag() const { return ptl_present_flag_; }
  void set_ptl_present_flag(bool value) { ptl_present_flag_ = value; }

  uint16_t ols_idx() const { return ols_idx_; }
  void set_ols_idx(uint16_t value) { ols_idx_ = value; }

  uint8_t num_sublayers() const { return num_sublayers_; }
  void set_num_sublayers(uint8_t value) { num_sublayers_ = value; }

  uint8_t constant_frame_rate() const { return constant_frame_rate_; }
  void set_constant_frame_rate(uint8_t value) { constant_frame_rate_ = value; }

  uint8_t chroma_format_idc() const { return chroma_format_idc_; }
  void set_chroma_format_idc(uint8_t value) { chroma_format_idc_ = value; }

  uint8_t bit_depth_minus8() const { return bit_depth_minus8_; }
  void set_bit_depth_minus8(uint8_t value) { bit_depth_minus8_ = value; }

  uint16_t max_picture_width() const { return max_picture_width_; }
  void set_max_picture_width(uint16_t value) { max_picture_width_ = value; }

  uint16_t max_picture_height() const { return max_picture_height_; }
  void set_max_picture_height(uint16_t value) { max_picture_height_ = value; }

  uint16_t avg_frame_rate() const { return avg_frame_rate_; }
  void set_avg_frame_rate(uint16_t value) { avg_frame_rate_ = value; }

  const VvcPTLRecord& ptl_record() const { return ptl_record_; }
  VvcPTLRecord& mutable_ptl_record() { return ptl_record_; }

  const std::vector<VvcNalArray>& arrays() const { return arrays_; }
  std::vector<VvcNalArray>& mutable_arrays() { return arrays_; }

  /// Parse depuis des NAL units brutes
  bool ParseFromNalUnits(const uint8_t* data, size_t size);

  /// Génère le codec string pour DASH/HLS
  std::string GetCodecString(FourCC codec_fourcc) const;

 private:
  bool ReadWriteInternal(BoxBuffer* buffer) override;
  size_t ComputeSizeInternal() override;

  // Parse les parameter sets
  bool ParseParameterSets();
  bool ParseVPS(const uint8_t* data, size_t size);
  bool ParseSPS(const uint8_t* data, size_t size);

  // Configuration selon ISO/IEC 14496-15:2022
  uint8_t length_size_minus_one_ = 3;  // 4 bytes par défaut
  bool ptl_present_flag_ = true;

  // Si ptl_present_flag == true
  uint16_t ols_idx_ = 0;
  uint8_t num_sublayers_ = 0;
  uint8_t constant_frame_rate_ = 0;
  uint8_t chroma_format_idc_ = 1;  // 4:2:0
  uint8_t bit_depth_minus8_ = 0;   // 8 bits
  uint8_t num_bytes_constraint_info_ = 0;

  VvcPTLRecord ptl_record_;

  uint16_t max_picture_width_ = 0;
  uint16_t max_picture_height_ = 0;
  uint16_t avg_frame_rate_ = 0;

  // Tableaux de NAL units
  std::vector<VvcNalArray> arrays_;

  // Données brutes pour parsing
  std::vector<uint8_t> vps_data_;
  std::vector<uint8_t> sps_data_;
  std::vector<uint8_t> pps_data_;
};

/// Visual Sample Entry pour VVC
/// Types supportés: 'vvc1', 'vvi1'
class VVCVisualSampleEntry : public VisualSampleEntry {
 public:
  DECLARE_BOX_METHODS(VVCVisualSampleEntry);

  const VVCDecoderConfigurationRecord& vvc_config() const { return vvc_config_; }
  VVCDecoderConfigurationRecord& mutable_vvc_config() { return vvc_config_; }

 private:
  bool ReadWriteInternal(BoxBuffer* buffer) override;
  size_t ComputeSizeInternal() override;

  VVCDecoderConfigurationRecord vvc_config_;
};

}  // namespace mp4
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP4_VVC_H_
