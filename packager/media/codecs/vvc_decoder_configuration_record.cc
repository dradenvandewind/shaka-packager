// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/codecs/vvc_decoder_configuration_record.h"

#include <absl/log/check.h>
#include <absl/log/log.h>
#include <absl/strings/escaping.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_join.h>

#include "packager/media/base/bit_reader.h"
#include "packager/media/base/buffer_reader.h"
#include "packager/media/base/buffer_writer.h"
#include "packager/media/base/rcheck.h"
#include "packager/media/codecs/vvc_parser.h"
#include "packager/utils/bytes_to_string_view.h"

namespace shaka {
namespace media {

namespace {

// NAL unit types pour VVC (ITU-T H.266)
enum VvcNalUnitType {
  kVvcNalVPS = 14,       // Video Parameter Set
  kVvcNalSPS = 15,       // Sequence Parameter Set
  kVvcNalPPS = 16,       // Picture Parameter Set
  kVvcNalPrefixAPS = 17, // Adaptation Parameter Set (prefix)
  kVvcNalSuffixAPS = 18, // Adaptation Parameter Set (suffix)
};

// Helper pour lire Exp-Golomb unsigned
uint32_t ReadUE(BitReader* reader) {
  int leading_zeros = 0;
  uint32_t bit;
  
  while (reader->bits_available() > 0) {
    if (!reader->ReadBits(1, &bit)) {
      return 0;
    }
    if (bit != 0) {
      break;
    }
    leading_zeros++;
    if (leading_zeros >= 32) return 0;
  }
  
  if (leading_zeros == 0) return 0;
  if (reader->bits_available() < static_cast<size_t>(leading_zeros)) return 0;
  
  uint32_t value = (1u << leading_zeros) - 1;
  if (!reader->ReadBits(leading_zeros, &bit)) {
    return 0;
  }
  value += bit;
  return value;
}
#if 0
// not use
// Helper pour lire Exp-Golomb signed
int32_t ReadSE(BitReader* reader) {
  uint32_t code_num = ReadUE(reader);
  if (code_num == 0) return 0;
  return (code_num & 1) ? static_cast<int32_t>((code_num + 1) / 2)
                        : -static_cast<int32_t>(code_num / 2);
}
#endif 

// Trim leading zeros from hex string
std::string TrimLeadingZeros(const std::string& str) {
  DCHECK_GT(str.size(), 0u);
  for (size_t i = 0; i < str.size(); ++i) {
    if (str[i] != '0') return str.substr(i);
  }
  return "0";
}

// Encode constraint flags for codec string
std::string EncodeConstraintFlags(const std::vector<uint8_t>& flags) {
  if (flags.empty()) return "";
  
  // Remove trailing zero bytes
  std::vector<uint8_t> trimmed = flags;
  while (!trimmed.empty() && trimmed.back() == 0) {
    trimmed.pop_back();
  }
  
  if (trimmed.empty()) return "";
  
  std::string result;
  for (uint8_t byte : trimmed) {
    result += TrimLeadingZeros(
        absl::BytesToHexString(byte_array_to_string_view(&byte, 1)));
    result += ".";
  }
  
  // Remove trailing dot
  if (!result.empty() && result.back() == '.') {
    result.pop_back();
  }
  
  return result;
}

}  // namespace

// ============================================================================
// VvcPTLRecord Implementation
// ============================================================================

bool VvcPTLRecord::Parse(const uint8_t* data,
                         size_t size,
                         uint8_t max_sublayers_minus1) {
  if (size < 2) {
    LOG(ERROR) << "PTL data too small: " << size;
    return false;
  }

  BitReader reader(data, size);

  // Parse profile_tier_level() selon ITU-T H.266 Section 7.3.2.3
  
  // olsInScope flag (1 bit)
  uint8_t ols_in_scope_val;
  RCHECK(reader.ReadBits(1, &ols_in_scope_val));
  ols_in_scope = ols_in_scope_val;

  // num_sublayers (3 bits)
  uint8_t num_sub_val;
  RCHECK(reader.ReadBits(3, &num_sub_val));
  num_sublayers = num_sub_val;

  // general_profile_idc (7 bits)
  RCHECK(reader.ReadBits(7, &general_profile_idc));

  // general_tier_flag (1 bit)
  uint8_t tier_val;
  RCHECK(reader.ReadBits(1, &tier_val));
  general_tier_flag = tier_val;

  // general_level_idc (8 bits)
  RCHECK(reader.ReadBits(8, &general_level_idc));

  // ptl_frame_only_constraint_flag (1 bit)
  uint8_t frame_only_val;
  RCHECK(reader.ReadBits(1, &frame_only_val));
  ptl_frame_only_constraint_flag = frame_only_val;

  // ptl_multilayer_enabled_flag (1 bit)
  uint8_t multilayer_val;
  RCHECK(reader.ReadBits(1, &multilayer_val));
  ptl_multilayer_enabled_flag = multilayer_val;

  // general_constraint_info
  if (num_bytes_constraint_info > 0) {
    general_constraint_info.resize(num_bytes_constraint_info);
    for (size_t i = 0; i < num_bytes_constraint_info; i++) {
      uint8_t byte;
      RCHECK(reader.ReadBits(8, &byte));
      general_constraint_info[i] = byte;
    }
  }

  // Sublayer level info
  if (max_sublayers_minus1 > 0) {
    ptl_sublayer_level_present_flag.resize(max_sublayers_minus1);

    // Read present flags
    for (uint8_t i = 0; i < max_sublayers_minus1; i++) {
      uint8_t flag;
      RCHECK(reader.ReadBits(1, &flag));
      ptl_sublayer_level_present_flag[i] = flag;
    }

    // Byte alignment
    while (reader.bit_position() % 8 != 0) {
      uint8_t zero;
      RCHECK(reader.ReadBits(1, &zero));
      if (zero != 0) {
        VLOG(1) << "Non-zero alignment bit in PTL";
      }
    }

    // Read sublayer levels
    sublayer_level_idc.resize(max_sublayers_minus1, 0);
    for (uint8_t i = 0; i < max_sublayers_minus1; i++) {
      if (ptl_sublayer_level_present_flag[i]) {
        uint8_t level;
        RCHECK(reader.ReadBits(8, &level));
        sublayer_level_idc[i] = level;
      }
    }
  }

  // num_sub_profiles (8 bits)
  RCHECK(reader.ReadBits(8, &num_sub_profiles));

  // general_sub_profile_idc array
  general_sub_profile_idc.resize(num_sub_profiles);
  for (uint8_t i = 0; i < num_sub_profiles; i++) {
    uint32_t sub_profile;
    RCHECK(reader.ReadBits(32, &sub_profile));
    general_sub_profile_idc[i] = sub_profile;
  }

  return true;
}

void VvcPTLRecord::Write(BufferWriter* writer, uint8_t max_sublayers_minus1) const {
  // olsInScope (1) + num_sublayers (3) + general_profile_idc (7) + general_tier_flag (1)
  // Pack into 2 bytes
  uint16_t byte0_1 = (static_cast<uint16_t>(ols_in_scope & 0x01) << 15) |
                      (static_cast<uint16_t>(num_sublayers & 0x07) << 12) |
                      (static_cast<uint16_t>(general_profile_idc & 0x7F) << 5) |
                      (static_cast<uint16_t>(general_tier_flag & 0x01) << 4);
  
  writer->AppendInt(static_cast<uint8_t>(byte0_1 >> 8));
  writer->AppendInt(static_cast<uint8_t>(byte0_1 & 0xFF));

  // general_level_idc
  writer->AppendInt(general_level_idc);

  // ptl_frame_only_constraint_flag + ptl_multilayer_enabled_flag + reserved(6)
  uint8_t flags = (ptl_frame_only_constraint_flag << 7) |
                  (ptl_multilayer_enabled_flag << 6);
  writer->AppendInt(flags);

  // general_constraint_info
  for (uint8_t byte : general_constraint_info) {
    writer->AppendInt(byte);
  }

  // Sublayer info
  if (max_sublayers_minus1 > 0) {
    // Pack present flags into bytes
    uint8_t current_byte = 0;
    int bit_pos = 7;

    for (size_t i = 0; i < max_sublayers_minus1; i++) {
      if (i < ptl_sublayer_level_present_flag.size() &&
          ptl_sublayer_level_present_flag[i]) {
        current_byte |= (1 << bit_pos);
      }
      bit_pos--;

      if (bit_pos < 0 || i == static_cast<size_t>(max_sublayers_minus1 - 1)) {
        writer->AppendInt(current_byte);
        current_byte = 0;
        bit_pos = 7;
      }
    }

    // Write sublayer levels
    for (size_t i = 0; i < max_sublayers_minus1; i++) {
      if (i < ptl_sublayer_level_present_flag.size() &&
          ptl_sublayer_level_present_flag[i] &&
          i < sublayer_level_idc.size()) {
        writer->AppendInt(sublayer_level_idc[i]);
      }
    }
  }

  // num_sub_profiles
  writer->AppendInt(num_sub_profiles);

  // general_sub_profile_idc array
  for (uint32_t sub_profile : general_sub_profile_idc) {
    writer->AppendInt(sub_profile);
  }
}

size_t VvcPTLRecord::ComputeSize(uint8_t max_sublayers_minus1) const {
  // Base: olsInScope + num_sublayers + profile + tier + level + flags (4 bytes)
  size_t size = 4;

  // Constraint info
  size += general_constraint_info.size();

  // Sublayer info
  if (max_sublayers_minus1 > 0) {
    // Present flags (rounded up to bytes)
    size += (max_sublayers_minus1 + 7) / 8;

    // Level values
    for (size_t i = 0; i < max_sublayers_minus1 &&
                       i < ptl_sublayer_level_present_flag.size(); i++) {
      if (ptl_sublayer_level_present_flag[i]) {
        size++;
      }
    }
  }

  // Sub-profiles
  size += 1;  // num_sub_profiles
  size += general_sub_profile_idc.size() * 4;

  return size;
}

// ============================================================================
// VvcDecoderConfigurationRecord Implementation
// ============================================================================

VvcDecoderConfigurationRecord::VvcDecoderConfigurationRecord() = default;
VvcDecoderConfigurationRecord::~VvcDecoderConfigurationRecord() = default;

bool VvcDecoderConfigurationRecord::ParseInternal() {
  BufferReader reader(data(), data_size());

  // Byte 0: reserved(5) + lengthSizeMinusOne(2)
  uint8_t byte0;
  RCHECK(reader.Read1(&byte0));

  // Vérifier les bits réservés (doivent être à 1)
  if ((byte0 & 0xFC) != 0xFC) {
    VLOG(1) << "Reserved bits not set to 1 in vvcC";
  }

  length_size_minus_one_ = byte0 & 0x03;
  set_nalu_length_size((length_size_minus_one_ & 0x03) + 1);

  // Byte 1: ptl_present_flag(1) + reserved(7)
  uint8_t byte1;
  RCHECK(reader.Read1(&byte1));
  ptl_present_flag_ = (byte1 & 0x80) != 0;

  if (ptl_present_flag_) {
    // Parse PTL information selon ISO/IEC 14496-15:2022

    // ols_idx(9) + num_sublayers(3) + constant_frame_rate(2) + chroma_format_idc(2)
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
    ptl_record_.num_bytes_constraint_info = constraint_byte & 0x3F;

    // Parse PTL record
    size_t ptl_start_pos = reader.pos();
    const uint8_t* ptl_data = reader.data() + ptl_start_pos;
    size_t ptl_max_size = reader.size() - ptl_start_pos;

    if (!ptl_record_.Parse(ptl_data, ptl_max_size, num_sublayers_)) {
      LOG(ERROR) << "Failed to parse VVC PTL record";
      return false;
    }

    // Skip PTL data
    size_t ptl_size = ptl_record_.ComputeSize(num_sublayers_);
    RCHECK(reader.SkipBytes(ptl_size));

    // max_picture_width, max_picture_height, avg_frame_rate
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

    // Number of NAL units in this array
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

      // Create Nalu object and add to internal list
      Nalu nalu;
      RCHECK(nalu.Initialize(Nalu::kH266, 
                            arrays_[i].nal_units[j].data(),
                            arrays_[i].nal_units[j].size()));
      RCHECK(nalu.type() == arrays_[i].nal_unit_type);
      AddNalu(nalu);

      // Save parameter sets for parsing
      if (arrays_[i].nal_unit_type == kVvcNalVPS && vps_data_.empty()) {
        vps_data_ = arrays_[i].nal_units[j];
      } else if (arrays_[i].nal_unit_type == kVvcNalSPS && sps_data_.empty()) {
        sps_data_ = arrays_[i].nal_units[j];
      } else if (arrays_[i].nal_unit_type == kVvcNalPPS && pps_data_.empty()) {
        pps_data_ = arrays_[i].nal_units[j];
      }
    }
  }

  return ParseParameterSets();
}

bool VvcDecoderConfigurationRecord::ParseParameterSets() {
  // Parse VPS if available (optional)
  if (!vps_data_.empty()) {
    if (!ParseVPS(vps_data_.data(), vps_data_.size())) {
      VLOG(1) << "Failed to parse VPS (non-critical)";
    }
  }

  // Parse SPS (required for dimensions and color info)
  if (!sps_data_.empty()) {
    if (!ParseSPS(sps_data_.data(), sps_data_.size())) {
      LOG(ERROR) << "Failed to parse SPS";
      return false;
    }
  }

  return true;
}

bool VvcDecoderConfigurationRecord::ParseVPS(const uint8_t* data, size_t size) {
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

bool VvcDecoderConfigurationRecord::ParseSPS(const uint8_t* data, size_t size) {
  if (size < 3) {
    LOG(ERROR) << "SPS too small: " << size;
    return false;
  }

  BitReader reader(data, size);

  // Skip NAL header (16 bits)
  reader.SkipBits(16);

  // Parse SPS selon ITU-T H.266 Section 7.3.2.2

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
    uint8_t tier_flag;
    RCHECK(reader.ReadBits(1, &tier_flag));
    ptl_record_.general_tier_flag = tier_flag;

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
    // Offsets are in chroma sample units
    uint32_t sub_width_c = (chroma_format_idc_ == 1 || chroma_format_idc_ == 2) ? 2 : 1;
    uint32_t sub_height_c = (chroma_format_idc_ == 1) ? 2 : 1;

    uint32_t crop_width = (left + right) * sub_width_c;
    uint32_t crop_height = (top + bottom) * sub_height_c;

    if (crop_width < pic_width && crop_height < pic_height) {
      max_picture_width_ = static_cast<uint16_t>(pic_width - crop_width);
      max_picture_height_ = static_cast<uint16_t>(pic_height - crop_height);
    }
  }

  // TODO: Parse VUI parameters for color information if needed
  // For now, color info should be provided externally or parsed from VUI

  LOG(INFO) << "Parsed VVC SPS: " << max_picture_width_ << "x" << max_picture_height_
            << ", chroma_format=" << static_cast<int>(chroma_format_idc_)
            << ", profile=" << static_cast<int>(ptl_record_.general_profile_idc)
            << ", level=" << static_cast<int>(ptl_record_.general_level_idc);

  return true;
}

std::string VvcDecoderConfigurationRecord::GetCodecString(FourCC codec_fourcc) const {
  // ISO/IEC 14496-15:2022 Annex E - VVC codec string format
  // Format: vvc1.PROFILE.TIER_LEVEL[.CONSTRAINTS]
  
  std::vector<std::string> fields;
  fields.push_back(FourCCToString(codec_fourcc));

  if (ptl_present_flag_) {
    // Profile
    fields.push_back(absl::StrFormat("%d", ptl_record_.general_profile_idc));

    // Tier and Level: [HL]NNN where H=High tier, L=Main tier, NNN=level_idc
    char tier_char = ptl_record_.general_tier_flag ? 'H' : 'L';
    fields.push_back(absl::StrFormat("%c%d", tier_char, ptl_record_.general_level_idc));

    // Constraints (optional)
    if (!ptl_record_.general_constraint_info.empty()) {
      std::string constraints = EncodeConstraintFlags(ptl_record_.general_constraint_info);
      if (!constraints.empty()) {
        fields.push_back("C" + constraints);
      }
    }
  }

  return absl::StrJoin(fields, ".");
}

bool VvcDecoderConfigurationRecord::ParseFromNalUnits(const std::vector<uint8_t>& nal_units_data) {
  H266Parser parser;
  std::vector<H266Parser::NalUnit> nal_units;

  if (!parser.ParseNalUnits(nal_units_data.data(),
                            nal_units_data.size(),
                            &nal_units)) {
    LOG(ERROR) << "Failed to parse VVC NAL units from bitstream";
    return false;
  }

  // Organize NAL units into arrays
  for (const auto& nalu_info : nal_units) {
    uint8_t nal_type = static_cast<uint8_t>(nalu_info.type);

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

    array->nal_units.emplace_back(nalu_info.data, nalu_info.data + nalu_info.size);

    // Create Nalu object
    Nalu nalu;
    if (nalu.Initialize(Nalu::kH266, nalu_info.data, nalu_info.size)) {
      AddNalu(nalu);
    }

    // Save parameter sets for parsing
    // if (nal_type == kVvcNalVPS) {
    //   vps_data_.assign(nalu_info.data, nalu_info.data + nalu_info.size);
    // } else if (nal_type == kVvcNalSPS) {
    //   sps_data_.assign(nalu_info.data, nalu_info.data + nalu_info.size);
    // } else if (nal_type == kVvcNalPPS) {
    //   pps_data_.assign(nalu_info.data, nalu_info.data + nalu_info.size);
    // }
 //quick workaround
 constexpr uint8_t kVvcNalUnitType_VPS = 14;      // Video Parameter Set
constexpr uint8_t kVvcNalUnitType_SPS = 15;      // Sequence Parameter Set  
constexpr uint8_t kVvcNalUnitType_PPS = 16;      // Picture Parameter Set

    if (nalu_info.type == kVvcNalUnitType_VPS) {
      vps_data_.assign(nalu_info.data, nalu_info.data + nalu_info.size);
    } else if (nalu_info.type == kVvcNalUnitType_SPS) {
      sps_data_.assign(nalu_info.data, nalu_info.data + nalu_info.size);
    } else if (nalu_info.type == kVvcNalUnitType_PPS) {
      pps_data_.assign(nalu_info.data, nalu_info.data + nalu_info.size);
    }
  }

  return ParseParameterSets();
}

void VvcDecoderConfigurationRecord::AddNalUnit(uint8_t nal_unit_type,
                                               const uint8_t* data,
                                               size_t size,
                                               bool array_completeness) {
  // Find or create array for this NAL type
  VvcNalArray* array = nullptr;
  for (auto& arr : arrays_) {
    if (arr.nal_unit_type == nal_unit_type) {
      array = &arr;
      break;
    }
  }

  if (!array) {
    arrays_.emplace_back();
    array = &arrays_.back();
    array->nal_unit_type = nal_unit_type;
    array->array_completeness = array_completeness ? 1 : 0;
  }

  array->nal_units.emplace_back(data, data + size);
}

bool VvcDecoderConfigurationRecord::GetVideoDimensions(uint32_t* width,
                                                        uint32_t* height) const {
  if (max_picture_width_ == 0 || max_picture_height_ == 0) {
    return false;
  }

  *width = max_picture_width_;
  *height = max_picture_height_;
  return true;
}

//VideoStreamInfo::ChromaSubsampling
//VvcDecoderConfigurationRecord::GetChromaSubsampling() const {

VvcChromaSubsampling VvcDecoderConfigurationRecord::GetChromaSubsampling() const {
  switch (chroma_format_idc_) {
    case 0:
      return VvcChromaSubsampling::kUnknown;
    case 1:
      return VvcChromaSubsampling::k420;
    case 2:
      return VvcChromaSubsampling::k422;
    case 3:
      return VvcChromaSubsampling::k444;
    default:
      LOG(WARNING) << "Unknown chroma_format_idc: "
                   << static_cast<int>(chroma_format_idc_);
      return VvcChromaSubsampling::kUnknown;
  }
}

}  // namespace media
}  // namespace shaka