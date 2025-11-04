// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/formats/mp4/vvc.h"

#include "packager/base/strings/string_number_conversions.h"
#include "packager/media/base/bit_reader.h"
#include "packager/media/base/rcheck.h"
#include "packager/media/codecs/vvc_parser.h"

namespace shaka {
namespace media {
namespace mp4 {

namespace {

// NAL unit types VVC
const uint8_t kVvcNalVPS = 14;
const uint8_t kVvcNalSPS = 15;
const uint8_t kVvcNalPPS = 16;

// Helper pour lire Exp-Golomb unsigned
uint32_t ReadUE(BitReader* reader) {
  int leading_zeros = 0;
  while (reader->bits_available() > 0 && !reader->ReadBits(1)) {
    leading_zeros++;
    if (leading_zeros >= 32) return 0;
  }
  if (leading_zeros == 0) return 0;
  if (reader->bits_available() < leading_zeros) return 0;
  uint32_t value = (1u << leading_zeros) - 1;
  value += reader->ReadBits(leading_zeros);
  return value;
}

}  // namespace

// ============================================================================
// VVCDecoderConfigurationRecord Implementation
// ============================================================================

VVCDecoderConfigurationRecord::VVCDecoderConfigurationRecord() = default;
VVCDecoderConfigurationRecord::~VVCDecoderConfigurationRecord() = default;

FOURCC_DEFINE(VVCDecoderConfigurationRecord, 'vvcC');

bool VVCDecoderConfigurationRecord::ReadWriteInternal(BoxBuffer* buffer) {
  RCHECK(ReadWriteHeaderInternal(buffer));

  if (buffer->Reading()) {
    // Parse vvcC box
    std::vector<uint8_t> data;
    RCHECK(buffer->ReadWriteVector(&data, buffer->remaining()));

    if (data.size() < 5) {
      LOG(ERROR) << "vvcC box too small: " << data.size();
      return false;
    }

    BufferReader reader(data.data(), data.size());

    // Byte 0: reserved(5) + lengthSizeMinusOne(2)
    uint8_t byte0;
    RCHECK(reader.Read1(&byte0));
    length_size_minus_one_ = byte0 & 0x03;

    // Byte 1: ptl_present_flag(1) + reserved(7)
    uint8_t byte1;
    RCHECK(reader.Read1(&byte1));
    ptl_present_flag_ = (byte1 & 0x80) != 0;

    if (ptl_present_flag_) {
      // Config word: ols_idx(9) + num_sublayers(3) + constant_frame_rate(2) + chroma_format(2)
      uint16_t config_word;
      RCHECK(reader.Read2(&config_word));
      ols_idx_ = (config_word >> 7) & 0x1FF;
      num_sublayers_ = (config_word >> 4) & 0x07;
      constant_frame_rate_ = (config_word >> 2) & 0x03;
      chroma_format_idc_ = config_word & 0x03;

      // bit_depth_minus8(3) + reserved(5)
      uint8_t bit_depth_byte;
      RCHECK(reader.Read1(&bit_depth_byte));
      bit_depth_minus8_ = (bit_depth_byte >> 5) & 0x07;

      // reserved(2) + num_bytes_constraint_info(6)
      uint8_t constraint_byte;
      RCHECK(reader.Read1(&constraint_byte));
      num_bytes_constraint_info_ = constraint_byte & 0x3F;

      // Parse PTL record
      // olsInScope(1) + num_sublayers(3) in first byte
      uint8_t ptl_byte0;
      RCHECK(reader.Read1(&ptl_byte0));
      ptl_record_.ols_in_scope = (ptl_byte0 >> 7) & 0x01;
      ptl_record_.num_sublayers = (ptl_byte0 >> 4) & 0x07;

      // general_profile_idc(7) + general_tier_flag(1) in next byte
      uint8_t ptl_byte1;
      RCHECK(reader.Read1(&ptl_byte1));
      ptl_record_.general_profile_idc = (ptl_byte1 >> 1) & 0x7F;
      ptl_record_.general_tier_flag = ptl_byte1 & 0x01;

      // general_level_idc(8)
      RCHECK(reader.Read1(&ptl_record_.general_level_idc));

      // ptl_frame_only_constraint_flag(1) + ptl_multilayer_enabled_flag(1) + reserved(6)
      uint8_t flags_byte;
      RCHECK(reader.Read1(&flags_byte));
      ptl_record_.ptl_frame_only_constraint_flag = (flags_byte >> 7) & 0x01;
      ptl_record_.ptl_multilayer_enabled_flag = (flags_byte >> 6) & 0x01;

      // general_constraint_info
      if (num_bytes_constraint_info_ > 0) {
        ptl_record_.general_constraint_info.resize(num_bytes_constraint_info_);
        for (size_t i = 0; i < num_bytes_constraint_info_; i++) {
          RCHECK(reader.Read1(&ptl_record_.general_constraint_info[i]));
        }
      }

      // Sublayer level info
      if (num_sublayers_ > 0) {
        // Read present flags (packed in bytes)
        size_t num_flag_bytes = (num_sublayers_ + 7) / 8;
        for (size_t i = 0; i < num_flag_bytes; i++) {
          uint8_t flag_byte;
          RCHECK(reader.Read1(&flag_byte));
          for (int j = 7; j >= 0 && ptl_record_.ptl_sublayer_level_present_flag.size() < num_sublayers_; j--) {
            ptl_record_.ptl_sublayer_level_present_flag.push_back((flag_byte >> j) & 0x01);
          }
        }

        // Read sublayer levels
        ptl_record_.sublayer_level_idc.resize(num_sublayers_, 0);
        for (size_t i = 0; i < num_sublayers_; i++) {
          if (i < ptl_record_.ptl_sublayer_level_present_flag.size() &&
              ptl_record_.ptl_sublayer_level_present_flag[i]) {
            RCHECK(reader.Read1(&ptl_record_.sublayer_level_idc[i]));
          }
        }
      }

      // num_sub_profiles
      RCHECK(reader.Read1(&ptl_record_.num_sub_profiles));

      // general_sub_profile_idc array
      ptl_record_.general_sub_profile_idc.resize(ptl_record_.num_sub_profiles);
      for (uint8_t i = 0; i < ptl_record_.num_sub_profiles; i++) {
        RCHECK(reader.Read4(&ptl_record_.general_sub_profile_idc[i]));
      }

      // Picture dimensions and frame rate
      RCHECK(reader.Read2(&max_picture_width_));
      RCHECK(reader.Read2(&max_picture_height_));
      RCHECK(reader.Read2(&avg_frame_rate_));
    }

    // Number of arrays
    uint8_t num_arrays;
    RCHECK(reader.Read1(&num_arrays));

    arrays_.resize(num_arrays);
    for (size_t i = 0; i < num_arrays; i++) {
      // Array header: array_completeness(1) + reserved(2) + NAL_unit_type(5)
      uint8_t array_header;
      RCHECK(reader.Read1(&array_header));
      arrays_[i].array_completeness = (array_header >> 7) & 0x01;
      arrays_[i].nal_unit_type = array_header & 0x1F;

      // Number of NAL units
      uint16_t num_nalus;
      RCHECK(reader.Read2(&num_nalus));

      arrays_[i].nal_units.resize(num_nalus);
      for (size_t j = 0; j < num_nalus; j++) {
        // NAL unit length
        uint16_t nal_size;
        RCHECK(reader.Read2(&nal_size));

        // NAL unit data
        arrays_[i].nal_units[j].resize(nal_size);
        RCHECK(reader.ReadToVector(&arrays_[i].nal_units[j], nal_size));

        // Save parameter sets
        if (arrays_[i].nal_unit_type == kVvcNalVPS && vps_data_.empty()) {
          vps_data_ = arrays_[i].nal_units[j];
        } else if (arrays_[i].nal_unit_type == kVvcNalSPS && sps_data_.empty()) {
          sps_data_ = arrays_[i].nal_units[j];
        } else if (arrays_[i].nal_unit_type == kVvcNalPPS && pps_data_.empty()) {
          pps_data_ = arrays_[i].nal_units[j];
        }
      }
    }

    RCHECK(ParseParameterSets());

  } else {
    // Write vvcC box
    RCHECK(buffer->PrepareChildren());

    // Byte 0: reserved(5) + lengthSizeMinusOne(2)
    uint8_t byte0 = 0xF8 | (length_size_minus_one_ & 0x03);
    RCHECK(buffer->ReadWriteUInt8(&byte0));

    // Byte 1: ptl_present_flag(1) + reserved(7)
    uint8_t byte1 = ptl_present_flag_ ? 0x80 : 0x00;
    RCHECK(buffer->ReadWriteUInt8(&byte1));

    if (ptl_present_flag_) {
      // Config word
      uint16_t config = (ols_idx_ << 7) |
                        ((num_sublayers_ & 0x07) << 4) |
                        ((constant_frame_rate_ & 0x03) << 2) |
                        (chroma_format_idc_ & 0x03);
      RCHECK(buffer->ReadWriteUInt16(&config));

      // bit_depth_minus8(3) + reserved(5)
      uint8_t bit_depth_byte = ((bit_depth_minus8_ & 0x07) << 5) | 0x1F;
      RCHECK(buffer->ReadWriteUInt8(&bit_depth_byte));

      // reserved(2) + num_bytes_constraint_info(6)
      uint8_t constraint_byte = num_bytes_constraint_info_ & 0x3F;
      RCHECK(buffer->ReadWriteUInt8(&constraint_byte));

      // Write PTL record
      // olsInScope(1) + num_sublayers(3) + reserved(4)
      uint8_t ptl_byte0 = (ptl_record_.ols_in_scope << 7) |
                          ((ptl_record_.num_sublayers & 0x07) << 4);
      RCHECK(buffer->ReadWriteUInt8(&ptl_byte0));

      // general_profile_idc(7) + general_tier_flag(1)
      uint8_t ptl_byte1 = (ptl_record_.general_profile_idc << 1) |
                          (ptl_record_.general_tier_flag & 0x01);
      RCHECK(buffer->ReadWriteUInt8(&ptl_byte1));

      // general_level_idc
      uint8_t level = ptl_record_.general_level_idc;
      RCHECK(buffer->ReadWriteUInt8(&level));

      // Flags
      uint8_t flags = (ptl_record_.ptl_frame_only_constraint_flag << 7) |
                      (ptl_record_.ptl_multilayer_enabled_flag << 6);
      RCHECK(buffer->ReadWriteUInt8(&flags));

      // general_constraint_info
      for (uint8_t byte : ptl_record_.general_constraint_info) {
        uint8_t constraint = byte;
        RCHECK(buffer->ReadWriteUInt8(&constraint));
      }

      // Sublayer info
      if (num_sublayers_ > 0) {
        // Pack present flags into bytes
        uint8_t current_byte = 0;
        int bit_pos = 7;

        for (size_t i = 0; i < num_sublayers_; i++) {
          if (i < ptl_record_.ptl_sublayer_level_present_flag.size() &&
              ptl_record_.ptl_sublayer_level_present_flag[i]) {
            current_byte |= (1 << bit_pos);
          }
          bit_pos--;

          if (bit_pos < 0 || i == num_sublayers_ - 1) {
            RCHECK(buffer->ReadWriteUInt8(&current_byte));
            current_byte = 0;
            bit_pos = 7;
          }
        }

        // Write sublayer levels
        for (size_t i = 0; i < num_sublayers_; i++) {
          if (i < ptl_record_.ptl_sublayer_level_present_flag.size() &&
              ptl_record_.ptl_sublayer_level_present_flag[i]) {
            if (i < ptl_record_.sublayer_level_idc.size()) {
              uint8_t level_val = ptl_record_.sublayer_level_idc[i];
              RCHECK(buffer->ReadWriteUInt8(&level_val));
            }
          }
        }
      }

      // num_sub_profiles
      uint8_t num_sub = ptl_record_.num_sub_profiles;
      RCHECK(buffer->ReadWriteUInt8(&num_sub));

      // general_sub_profile_idc array
      for (uint32_t sub_profile : ptl_record_.general_sub_profile_idc) {
        uint32_t val = sub_profile;
        RCHECK(buffer->ReadWriteUInt32(&val));
      }

      // Picture dimensions and frame rate
      uint16_t width = max_picture_width_;
      uint16_t height = max_picture_height_;
      uint16_t frame_rate = avg_frame_rate_;
      RCHECK(buffer->ReadWriteUInt16(&width));
      RCHECK(buffer->ReadWriteUInt16(&height));
      RCHECK(buffer->ReadWriteUInt16(&frame_rate));
    }

    // Number of arrays
    uint8_t num_arrays = static_cast<uint8_t>(arrays_.size());
    RCHECK(buffer->ReadWriteUInt8(&num_arrays));

    // Write each array
    for (auto& array : arrays_) {
      // Array header: array_completeness(1) + reserved(2) + NAL_unit_type(5)
      uint8_t header = (array.array_completeness << 7) | (array.nal_unit_type & 0x1F);
      RCHECK(buffer->ReadWriteUInt8(&header));

      // Number of NAL units
      uint16_t num_nalus = static_cast<uint16_t>(array.nal_units.size());
      RCHECK(buffer->ReadWriteUInt16(&num_nalus));

      // Write each NAL unit
      for (auto& nalu : array.nal_units) {
        uint16_t nal_size = static_cast<uint16_t>(nalu.size());
        RCHECK(buffer->ReadWriteUInt16(&nal_size));
        RCHECK(buffer->ReadWriteVector(&nalu, nal_size));
      }
    }
  }

  return true;
}

size_t VVCDecoderConfigurationRecord::ComputeSizeInternal() {
  size_t size = HeaderSize() + 2;  // Box header + base config (2 bytes)

  if (ptl_present_flag_) {
    size += 4;  // config_word + bit_depth + constraint_info_size
    size += 4;  // PTL base (ols_in_scope + num_sublayers + profile + tier + level + flags)
    size += num_bytes_constraint_info_;  // Constraint info bytes

    // Sublayer info
    if (num_sublayers_ > 0) {
      size += (num_sublayers_ + 7) / 8;  // Present flags (packed)
      
      // Count sublayer levels that will be written
      for (size_t i = 0; i < num_sublayers_ && 
                         i < ptl_record_.ptl_sublayer_level_present_flag.size(); i++) {
        if (ptl_record_.ptl_sublayer_level_present_flag[i]) {
          size++;
        }
      }
    }

    // Sub-profiles
    size += 1;  // num_sub_profiles
    size += ptl_record_.general_sub_profile_idc.size() * 4;

    // Picture info
    size += 6;  // max_picture_width(2) + max_picture_height(2) + avg_frame_rate(2)
  }

  // Arrays
  size += 1;  // num_arrays

  for (const auto& array : arrays_) {
    size += 1;  // array header
    size += 2;  // num_nalus

    for (const auto& nalu : array.nal_units) {
      size += 2;  // nal_unit_length
      size += nalu.size();
    }
  }

  return size;
}

bool VVCDecoderConfigurationRecord::ParseFromNalUnits(const uint8_t* data, size_t size) {
  VvcParser parser;
  std::vector<VvcParser::NalUnit> nal_units;

  if (!parser.ParseNalUnits(data, size, &nal_units)) {
    LOG(ERROR) << "Failed to parse NAL units from bitstream";
    return false;
  }

  // Organize NAL units into arrays
  for (const auto& nalu : nal_units) {
    uint8_t nal_type = static_cast<uint8_t>(nalu.type);

    // Find or create array for this NAL type
    VvcNalArray* array = nullptr;
    for (auto& arr : arrays_) {
      if (arr.nal_unit_type == nal_type) {
        array = &arr;
        break;
      }
    }

    if (!array) {
      arrays_.emplace_back();
      array = &arrays_.back();
      array->nal_unit_type = nal_type;
      array->array_completeness = 1;  // Assume complete
    }

    array->nal_units.emplace_back(nalu.data, nalu.data + nalu.size);

    // Save parameter sets for parsing
    if (nal_type == kVvcNalVPS) {
      vps_data_.assign(nalu.data, nalu.data + nalu.size);
    } else if (nal_type == kVvcNalSPS) {
      sps_data_.assign(nalu.data, nalu.data + nalu.size);
    } else if (nal_type == kVvcNalPPS) {
      pps_data_.assign(nalu.data, nalu.data + nalu.size);
    }
  }

  return ParseParameterSets();
}

std::string VVCDecoderConfigurationRecord::GetCodecString(FourCC codec_fourcc) const {
  std::string codec = FourCCToString(codec_fourcc);

  if (ptl_present_flag_) {
    char tier = ptl_record_.general_tier_flag ? 'H' : 'L';
    
    // Format: vvc1.PROFILE.TIER_LEVEL
    codec += base::StringPrintf(".%u.%c%u",
                                 ptl_record_.general_profile_idc,
                                 tier,
                                 ptl_record_.general_level_idc);

    // Add constraints if present (optional)
    if (!ptl_record_.general_constraint_info.empty()) {
      codec += ".C";
      for (uint8_t byte : ptl_record_.general_constraint_info) {
        codec += base::HexEncode(&byte, 1);
      }
    }
  }

  return codec;
}

bool VVCDecoderConfigurationRecord::ParseParameterSets() {
  bool success = true;

  // Parse VPS if available
  if (!vps_data_.empty()) {
    if (!ParseVPS(vps_data_.data(), vps_data_.size())) {
      LOG(WARNING) << "Failed to parse VPS (non-critical)";
      success = false;
    }
  }

  // Parse SPS (required for dimensions and other info)
  if (!sps_data_.empty()) {
    if (!ParseSPS(sps_data_.data(), sps_data_.size())) {
      LOG(ERROR) << "Failed to parse SPS";
      return false;
    }
  }

  return success;
}

bool VVCDecoderConfigurationRecord::ParseVPS(const uint8_t* data, size_t size) {
  if (size < 3) {
    LOG(WARNING) << "VPS too small: " << size;
    return false;
  }

  BitReader reader(data, size);

  // Skip NAL header (16 bits)
  reader.SkipBits(16);

  // vps_video_parameter_set_id (4 bits)
  uint8_t vps_id;
  RCHECK(reader.ReadBits(4, &vps_id));

  // vps_max_layers_minus1 (6 bits)
  uint8_t max_layers_minus1;
  RCHECK(reader.ReadBits(6, &max_layers_minus1));

  // vps_max_sublayers_minus1 (3 bits)
  uint8_t max_sublayers_minus1;
  RCHECK(reader.ReadBits(3, &max_sublayers_minus1));

  VLOG(2) << "Parsed VPS: id=" << static_cast<int>(vps_id)
          << ", max_layers=" << static_cast<int>(max_layers_minus1 + 1)
          << ", max_sublayers=" << static_cast<int>(max_sublayers_minus1 + 1);

  return true;
}

bool VVCDecoderConfigurationRecord::ParseSPS(const uint8_t* data, size_t size) {
  if (size < 3) {
    LOG(ERROR) << "SPS too small: " << size;
    return false;
  }

  BitReader reader(data, size);

  // Skip NAL header (16 bits)
  reader.SkipBits(16);

  // sps_seq_parameter_set_id (ue(v))
  uint32_t sps_id = ReadUE(&reader);
  if (sps_id > 15) {
    LOG(ERROR) << "Invalid sps_seq_parameter_set_id: " << sps_id;
    return false;
  }

  // sps_video_parameter_set_id (4 bits)
  uint8_t vps_id;
  RCHECK(reader.ReadBits(4, &vps_id));

  // sps_max_sublayers_minus1 (3 bits)
  uint8_t max_sublayers;
  RCHECK(reader.ReadBits(3, &max_sublayers));
  num_sublayers_ = max_sublayers;

  // sps_chroma_format_idc (2 bits)
  RCHECK(reader.ReadBits(2, &chroma_format_idc_));

  // sps_log2_ctu_size_minus5 (2 bits)
  uint8_t log2_ctu_size_minus5;
  RCHECK(reader.ReadBits(2, &log2_ctu_size_minus5));

  // sps_ptl_dpb_hrd_params_present_flag (1 bit)
  uint8_t params_present;
  RCHECK(reader.ReadBits(1, &params_present));

  if (params_present && !ptl_present_flag_) {
    // Parse embedded profile_tier_level()
    ptl_present_flag_ = true;

    // olsInScope (1 bit)
    reader.SkipBits(1);

    // num_sublayers (3 bits)
    reader.SkipBits(3);

    // general_profile_idc (7 bits)
    RCHECK(reader.ReadBits(7, &ptl_record_.general_profile_idc));

    // general_tier_flag (1 bit)
    RCHECK(reader.ReadBits(1, &ptl_record_.general_tier_flag));

    // general_level_idc (8 bits)
    RCHECK(reader.ReadBits(8, &ptl_record_.general_level_idc));
  }

  // sps_gdr_enabled_flag (1 bit)
  reader.SkipBits(1);

  // sps_ref_pic_resampling_enabled_flag (1 bit)
  uint8_t resampling_enabled;
  RCHECK(reader.ReadBits(1, &resampling_enabled));

  // sps_res_change_in_clvs_allowed_flag (1 bit) - conditional
  if (resampling_enabled) {
    reader.SkipBits(1);
  }

  // sps_pic_width_max_in_luma_samples (ue(v))
  uint32_t pic_width = ReadUE(&reader);

  // sps_pic_height_max_in_luma_samples (ue(v))
  uint32_t pic_height = ReadUE(&reader);

  // Store dimensions
  max_picture_width_ = static_cast<uint16_t>(std::min(pic_width, 65535u));
  max_picture_height_ = static_cast<uint16_t>(std::min(pic_height, 65535u));

  // sps_conformance_window_flag (1 bit)
  uint8_t conformance_window_flag;
  RCHECK(reader.ReadBits(1, &conformance_window_flag));

  if (conformance_window_flag) {
    // conf_win_left_offset (ue(v))
    uint32_t left = ReadUE(&reader);
    // conf_win_right_offset (ue(v))
    uint32_t right = ReadUE(&reader);
    // conf_win_top_offset (ue(v))
    uint32_t top = ReadUE(&reader);
    // conf_win_bottom_offset (ue(v))
    uint32_t bottom = ReadUE(&reader);

    // Apply conformance window (crop)
    uint32_t sub_width_c = (chroma_format_idc_ == 1 || chroma_format_idc_ == 2) ? 2 : 1;
    uint32_t sub_height_c = (chroma_format_idc_ == 1) ? 2 : 1;

    uint32_t crop_width = (left + right) * sub_width_c;
    uint32_t crop_height = (top + bottom) * sub_height_c;

    if (crop_width < pic_width && crop_height < pic_height) {
      max_picture_width_ = static_cast<uint16_t>(pic_width - crop_width);
      max_picture_height_ = static_cast<uint16_t>(pic_height - crop_height);
    }
  }

  LOG(INFO) << "Parsed SPS: " << max_picture_width_ << "x" << max_picture_height_
            << ", chroma=" << static_cast<int>(chroma_format_idc_)
            << ", profile=" << static_cast<int>(ptl_record_.general_profile_idc)
            << ", level=" << static_cast<int>(ptl_record_.general_level_idc);

  return true;
}

// ============================================================================
// VVCVisualSampleEntry Implementation
// ============================================================================

VVCVisualSampleEntry::VVCVisualSampleEntry() = default;
VVCVisualSampleEntry::~VVCVisualSampleEntry() = default;

FourCC VVCVisualSampleEntry::BoxType() const {
  return format;
}

bool VVCVisualSampleEntry::ReadWriteInternal(BoxBuffer* buffer) {
  // First read/write the VisualSampleEntry base class
  RCHECK(VisualSampleEntry::ReadWriteInternal(buffer));

  // Then read/write the vvcC configuration
  if (buffer->Reading()) {
    RCHECK(buffer->PrepareChildren());
    RCHECK(buffer->ReadWriteChild(&vvc_config_));
  } else {
    RCHECK(buffer->ReadWriteChild(&vvc_config_));
  }

  return true;
}

size_t VVCVisualSampleEntry::ComputeSizeInternal() {
  return VisualSampleEntry::ComputeSizeInternal() +
         vvc_config_.ComputeSize();
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka