// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/codecs/vvc_decoder_configuration_record.h"

#include <algorithm>
#include <limits>

#include "packager/base/strings/string_number_conversions.h"
#include "packager/base/strings/string_util.h"
#include "packager/media/base/bit_reader.h"
#include "packager/media/base/buffer_reader.h"
#include "packager/media/base/buffer_writer.h"
#include "packager/media/base/rcheck.h"
#include "packager/media/codecs/vvc_parser.h"

namespace shaka {
namespace media {

namespace {

// NAL unit types pour VVC (ISO/IEC 23090-3)
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
  while (reader->bits_available() > 0 && !reader->ReadBits(1)) {
    leading_zeros++;
    if (leading_zeros >= 32) {
      return 0;  // Éviter l'overflow
    }
  }
  
  if (leading_zeros == 0) {
    return 0;
  }
  
  if (reader->bits_available() < leading_zeros) {
    return 0;
  }
  
  uint32_t value = (1u << leading_zeros) - 1;
  value += reader->ReadBits(leading_zeros);
  return value;
}

// Helper pour lire Exp-Golomb signed
int32_t ReadSE(BitReader* reader) {
  uint32_t code_num = ReadUE(reader);
  if (code_num == 0) {
    return 0;
  }
  return (code_num & 1) ? static_cast<int32_t>((code_num + 1) / 2) 
                        : -static_cast<int32_t>(code_num / 2);
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
  
  // Parse profile_tier_level() selon ISO/IEC 23090-3 Section 7.3.2.3
  
  // olsInScope flag (1 bit)
  uint8_t ols_in_scope;
  RCHECK(reader.ReadBits(1, &ols_in_scope));
  
  // num_sublayers (3 bits)
  uint8_t num_sub;
  RCHECK(reader.ReadBits(3, &num_sub));
  
  // general_profile_idc (7 bits)
  RCHECK(reader.ReadBits(7, &general_profile_idc));
  
  // general_tier_flag (1 bit)
  RCHECK(reader.ReadBits(1, &general_tier_flag));
  
  // general_level_idc (8 bits)
  RCHECK(reader.ReadBits(8, &general_level_idc));
  
  // ptl_frame_only_constraint_flag (1 bit)
  RCHECK(reader.ReadBits(1, &ptl_frame_only_constraint_flag));
  
  // ptl_multilayer_enabled_flag (1 bit)
  RCHECK(reader.ReadBits(1, &ptl_multilayer_enabled_flag));
  
  // general_constraint_info
  if (num_bytes_constraint_info > 0) {
    general_constraint_info.resize(num_bytes_constraint_info);
    for (size_t i = 0; i < num_bytes_constraint_info; i++) {
      uint8_t byte;
      RCHECK(reader.ReadBits(8, &byte));
      general_constraint_info[i] = byte;
    }
  }
  
  // Sublayer information
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
        LOG(WARNING) << "Non-zero alignment bit in PTL";
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
  uint16_t byte0_1 = (1 << 15) |  // olsInScope = 1
                      (static_cast<uint16_t>(max_sublayers_minus1) << 12) |
                      (static_cast<uint16_t>(general_profile_idc) << 5) |
                      (general_tier_flag ? 0x10 : 0x00);
  writer->AppendInt(static_cast<uint8_t>(byte0_1 >> 8));
  writer->AppendInt(static_cast<uint8_t>(byte0_1 & 0xFF));
  
  // general_level_idc
  writer->AppendInt(general_level_idc);
  
  // ptl_frame_only_constraint_flag + ptl_multilayer_enabled_flag + reserved
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
    
    for (size_t i = 0; i < ptl_sublayer_level_present_flag.size() && 
                       i < max_sublayers_minus1; i++) {
      if (ptl_sublayer_level_present_flag[i]) {
        current_byte |= (1 << bit_pos);
      }
      bit_pos--;
      
      if (bit_pos < 0 || i == max_sublayers_minus1 - 1) {
        writer->AppendInt(current_byte);
        current_byte = 0;
        bit_pos = 7;
      }
    }
    
    // Write sublayer levels
    for (size_t i = 0; i < sublayer_level_idc.size() && 
                       i < max_sublayers_minus1; i++) {
      if (i < ptl_sublayer_level_present_flag.size() &&
          ptl_sublayer_level_present_flag[i]) {
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
  // Base: olsInScope + num_sublayers + profile + tier + level + flags
  size_t size = 4;
  
  // Constraint info
  size += general_constraint_info.size();
  
  // Sublayer info
  if (max_sublayers_minus1 > 0) {
    // Present flags (rounded up to bytes)
    size += (max_sublayers_minus1 + 7) / 8;
    
    // Level values
    for (size_t i = 0; i < ptl_sublayer_level_present_flag.size() && 
                       i < max_sublayers_minus1; i++) {
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

bool VvcDecoderConfigurationRecord::Parse(const uint8_t* data, size_t size) {
  if (size < 5) {
    LOG(ERROR) << "VvcDecoderConfigurationRecord too small: " << size;
    return false;
  }
  
  BufferReader reader(data, size);
  
  // Byte 0: reserved(5) + lengthSizeMinusOne(2)
  uint8_t byte0;
  RCHECK(reader.Read1(&byte0));
  
  // Vérifier les bits réservés
  if ((byte0 & 0xF8) != 0xF8) {
    LOG(WARNING) << "Reserved bits not set to 1 in vvcC";
  }
  
  length_size_minus_one_ = byte0 & 0x03;
  
  // Byte 1: ptl_present_flag(1) + reserved(7)  
  uint8_t byte1;
  RCHECK(reader.Read1(&byte1));
  ptl_present_flag_ = (byte1 & 0x80) != 0;
  
  if (ptl_present_flag_) {
    // Parse PTL information
    
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
      LOG(ERROR) << "Failed to parse PTL record";
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
      
      // Sauvegarder les parameter sets pour parsing ultérieur
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

bool VvcDecoderConfigurationRecord::ParseFromNalUnits(
    const std::vector<uint8_t>& nal_units_data) {
  
  VvcParser parser;
  std::vector<VvcParser::NalUnit> nal_units;
  
  if (!parser.ParseNalUnits(nal_units_data.data(), 
                            nal_units_data.size(),
                            &nal_units)) {
    LOG(ERROR) << "Failed to parse NAL units from bitstream";
    return false;
  }
  
  // Extraire et organiser les NAL units
  for (const auto& nalu : nal_units) {
    uint8_t nal_type = static_cast<uint8_t>(nalu.type);
    
    AddNalUnit(nal_type, nalu.data, nalu.size);
    
    // Sauvegarder les parameter sets
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

void VvcDecoderConfigurationRecord::AddNalUnit(uint8_t nal_unit_type,
                                               const uint8_t* data,
                                               size_t size,
                                               bool array_completeness) {
  // Trouver ou créer l'array pour ce type de NAL
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

bool VvcDecoderConfigurationRecord::ParseParameterSets() {
  bool success = true;
  
  // Parser VPS si disponible
  if (!vps_data_.empty()) {
    if (!ParseVPS(vps_data_.data(), vps_data_.size())) {
      LOG(WARNING) << "Failed to parse VPS";
      success = false;
    }
  }
  
  // Parser SPS (requis pour les dimensions et autres infos)
  if (!sps_data_.empty()) {
    if (!ParseSPS(sps_data_.data(), sps_data_.size())) {
      LOG(ERROR) << "Failed to parse SPS";
      return false;
    }
  }
  
  // Parser PPS si nécessaire
  if (!pps_data_.empty()) {
    // Le parsing PPS peut être ajouté si besoin
    VLOG(2) << "PPS available but not parsed (not critical)";
  }
  
  return success;
}

bool VvcDecoderConfigurationRecord::ParseSPS(const uint8_t* data, size_t size) {
  if (size < 3) {
    LOG(ERROR) << "SPS too small: " << size;
    return false;
  }
  
  BitReader reader(data, size);
  
  // Skip NAL unit header (16 bits)
  reader.SkipBits(16);
  
  // Parse SPS selon ISO/IEC 23090-3 Section 7.3.2.2
  
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
    // Parse profile_tier_level()
    ptl_present_flag_ = true;
    
    // olsInScope (1 bit)
    uint8_t ols_in_scope;
    RCHECK(reader.ReadBits(1, &ols_in_scope));
    
    // num_sublayers (3 bits)
    uint8_t num_sub;
    RCHECK(reader.ReadBits(3, &num_sub));
    
    // general_profile_idc (7 bits)
    RCHECK(reader.ReadBits(7, &ptl_record_.general_profile_idc));
    
    // general_tier_flag (1 bit)
    RCHECK(reader.ReadBits(1, &ptl_record_.general_tier_flag));
    
    // general_level_idc (8 bits)
    RCHECK(reader.ReadBits(8, &ptl_record_.general_level_idc));
    
    // ptl_frame_only_constraint_flag (1 bit)
    RCHECK(reader.ReadBits(1, &ptl_record_.ptl_frame_only_constraint_flag));
    
    // ptl_multilayer_enabled_flag (1 bit)
    RCHECK(reader.ReadBits(1, &ptl_record_.ptl_multilayer_enabled_flag));
    
    // Pour simplifier, on skip le reste des constraint info
    // Dans une implémentation complète, il faudrait parser general_constraint_info
  }
  
  // sps_gdr_enabled_flag (1 bit)
  uint8_t gdr_enabled;
  RCHECK(reader.ReadBits(1, &gdr_enabled));
  
  // sps_ref_pic_resampling_enabled_flag (1 bit)
  uint8_t resampling_enabled;
  RCHECK(reader.ReadBits(1, &resampling_enabled));
  
  // sps_res_change_in_clvs_allowed_flag (1 bit) - conditionnel
  if (resampling_enabled) {
    uint8_t res_change_allowed;
    RCHECK(reader.ReadBits(1, &res_change_allowed));
  }
  
  // sps_pic_width_max_in_luma_samples (ue(v))
  uint32_t pic_width = ReadUE(&reader);
  
  // sps_pic_height_max_in_luma_samples (ue(v))
  uint32_t pic_height = ReadUE(&reader);
  
  // Sauvegarder les dimensions
  max_picture_width_ = static_cast<uint16_t>(
      std::min(pic_width, static_cast<uint32_t>(65535)));
  max_picture_height_ = static_cast<uint16_t>(
      std::min(pic_height, static_cast<uint32_t>(65535)));
  
  // sps_conformance_window_flag (1 bit)
  uint8_t conformance_window_flag;
  RCHECK(reader.ReadBits(1, &conformance_window_flag));
  
  if (conformance_window_flag) {
    // conf_win_left_offset (ue(v))
    uint32_t left_offset = ReadUE(&reader);
    // conf_win_right_offset (ue(v))
    uint32_t right_offset = ReadUE(&reader);
    // conf_win_top_offset (ue(v))
    uint32_t top_offset = ReadUE(&reader);
    // conf_win_bottom_offset (ue(v))
    uint32_t bottom_offset = ReadUE(&reader);
    
    // Ajuster les dimensions selon la conformance window
    // Les offsets sont en unités de chroma selon chroma_format_idc
    uint32_t sub_width_c = (chroma_format_idc_ == 1 || chroma_format_idc_ == 2) ? 2 : 1;
    uint32_t sub_height_c = (chroma_format_idc_ == 1) ? 2 : 1;
    
    uint32_t crop_width = (left_offset + right_offset) * sub_width_c;
    uint32_t crop_height = (top_offset + bottom_offset) * sub_height_c;
    
    if (crop_width < pic_width && crop_height < pic_height) {
      max_picture_width_ = static_cast<uint16_t>(pic_width - crop_width);
      max_picture_height_ = static_cast<uint16_t>(pic_height - crop_height);
    }
  }
  
  // Le reste du SPS n'est pas critique pour notre usage
  // On peut arrêter le parsing ici
  
  LOG(INFO) << "Parsed SPS: " << max_picture_width_ << "x" << max_picture_height_
            << ", chroma_format=" << static_cast<int>(chroma_format_idc_)
            << ", profile=" << static_cast<int>(ptl_record_.general_profile_idc)
            << ", level=" << static_cast<int>(ptl_record_.general_level_idc);
  
  return true;
}

bool VvcDecoderConfigurationRecord::ParseVPS(const uint8_t* data, size_t size) {
  if (size < 3) {
    LOG(WARNING) << "VPS too small: " << size;
    return false;
  }
  
  BitReader reader(data, size);
  
  // Skip NAL unit header (16 bits)
  reader.SkipBits(16);
  
  // Parse VPS selon ISO/IEC 23090-3 Section 7.3.2.1
  
  // vps_video_parameter_set_id (4 bits)
  uint8_t vps_id;
  RCHECK(reader.ReadBits(4, &vps_id));
  
  // vps_max_layers_minus1 (6 bits)
  uint8_t max_layers_minus1;
  RCHECK(reader.ReadBits(6, &max_layers_minus1));
  
  // vps_max_sublayers_minus1 (3 bits)
  uint8_t max_sublayers_minus1;
  RCHECK(reader.ReadBits(3, &max_sublayers_minus1));
  
  // Le reste du VPS n'est pas critique pour la configuration de base
  // Dans une implémentation complète, on pourrait extraire les OLS, etc.
  
  VLOG(2) << "Parsed VPS: id=" << static_cast<int>(vps_id)
          << ", max_layers=" << static_cast<int>(max_layers_minus1 + 1)
          << ", max_sublayers=" << static_cast<int>(max_sublayers_minus1 + 1);
  
  return true;
}

void VvcDecoderConfigurationRecord::Write(BufferWriter* writer) const {
  // Byte 0: reserved(5) + lengthSizeMinusOne(2)
  writer->AppendInt(static_cast<uint8_t>(0xF8 | (length_size_minus_one_ & 0x03)));
  
  // Byte 1: ptl_present_flag(1) + reserved(7)
  writer->AppendInt(static_cast<uint8_t>(ptl_present_flag_ ? 0x80 : 0x00));
  
  if (ptl_present_flag_) {
    // ols_idx(9) + num_sublayers(3) + constant_frame_rate(2) + chroma_format_idc(2)
    uint16_t config = (ols_idx_ << 7) | 
                      ((num_sublayers_ & 0x07) << 4) |
                      ((constant_frame_rate_ & 0x03) << 2) | 
                      (chroma_format_idc_ & 0x03);
    writer->AppendInt(config);
    
    // bit_depth_minus8(3) + reserved(5)
    writer->AppendInt(static_cast<uint8_t>(((bit_depth_minus8_ & 0x07) << 5) | 0x1F));
    
    // reserved(2) + num_bytes_constraint_info(6)
    writer->AppendInt(static_cast<uint8_t>(ptl_record_.num_bytes_constraint_info & 0x3F));
    
    // Write PTL record
    ptl_record_.Write(writer, num_sublayers_);
    
    // Picture dimensions and frame rate
    writer->AppendInt(max_picture_width_);
    writer->AppendInt(max_picture_height_);
    writer->AppendInt(avg_frame_rate_);
  }
  
  // Number of arrays
  writer->AppendInt(static_cast<uint8_t>(arrays_.size()));
  
  for (const auto& array : arrays_) {
    // Array header: array_completeness(1) + reserved(2) + NAL_unit_type(5)
    uint8_t header = ((array.array_completeness & 0x01) << 7) | 
                     (array.nal_unit_type & 0x1F);
    writer->AppendInt(header);
    
    // Number of NAL units
    writer->AppendInt(static_cast<uint16_t>(array.nal_units.size()));
    
    for (const auto& nalu : array.nal_units) {
      // NAL unit length
      writer->AppendInt(static_cast<uint16_t>(nalu.size()));
      // NAL unit data
      writer->AppendVector(nalu);
    }
  }
}

size_t VvcDecoderConfigurationRecord::ComputeSize() const {
  size_t size = 2;  // Base: lengthSizeMinusOne + ptl_present_flag
  
  if (ptl_present_flag_) {
    size += 4;  // config word + bit_depth + constraint_info_size
    size += ptl_record_.ComputeSize(num_sublayers_);
    size += 6;  // max_picture_width + max_picture_height + avg_frame_rate
  }
  
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

std::string VvcDecoderConfigurationRecord::GetCodecString(
    FourCC codec_fourcc) const {
  
  std::string codec_string = FourCCToString(codec_fourcc);
  
  if (ptl_present_flag_) {
    // Format: vvc1.PROFILE.TIER_LEVEL[.CONSTRAINTS]
    // Exemples:
    // - vvc1.1.L93 : Profile Main 10, Main tier, Level 3.1
    // - vvc1.1.H153 : Profile Main 10, High tier, Level 5.1
    
    char tier_char = ptl_record_.general_tier_flag ? 'H' : 'L';
    
    codec_string += base::StringPrintf(
        ".%u.%c%u",
        ptl_record_.general_profile_idc,
        tier_char,
        ptl_record_.general_level_idc);
    
    // Ajouter les constraint flags si présents (optionnel)
    if (!ptl_record_.general_constraint_info.empty()) {
      codec_string += ".C";
      // Encoder les contraintes en hexadécimal
      for (uint8_t byte : ptl_record_.general_constraint_info) {
        codec_string += base::HexEncode(&byte, 1);
      }
    }
  }
  
  return codec_string;
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

VideoStreamInfo::ChromaSubsampling 
VvcDecoderConfigurationRecord::GetChromaSubsampling() const {
  switch (chroma_format_idc_) {
    case 0:
      return VideoStreamInfo::kMonochrome;
    case 1:
      return VideoStreamInfo::k420;
    case 2:
      return VideoStreamInfo::k422;
    case 3:
      return VideoStreamInfo::k444;
    default:
      LOG(WARNING) << "Unknown chroma_format_idc: " 
                   << static_cast<int>(chroma_format_idc_);
      return VideoStreamInfo::kUnknownChromaSubsampling;
  }
}

}  // namespace media
}  // namespace shaka