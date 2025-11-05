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
#include "packager/media/codecs/decoder_configuration_record.h"

namespace shaka {
namespace media {

class BufferReader;
class BufferWriter;

enum class VvcChromaSubsampling {
  kUnknown = 0,
  k420 = 1,  // 4:2:0
  k422 = 2,  // 4:2:2
  k444 = 3,  // 4:4:4
};

/// Structure représentant le PTL (Profile, Tier, Level) VVC
/// Selon ISO/IEC 14496-15:2022 Section 8.3.3.1.2 et ITU-T H.266 Section 7.3.2.3
struct VvcPTLRecord {
  uint8_t ols_in_scope = 1;
  uint8_t num_sublayers = 0;
  uint8_t num_bytes_constraint_info = 0;
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

  /// Parse PTL depuis un buffer
  /// @param data Buffer contenant les données PTL
  /// @param size Taille du buffer
  /// @param max_sublayers_minus1 Nombre maximum de sublayers - 1
  /// @return true si le parsing a réussi
  bool Parse(const uint8_t* data, size_t size, uint8_t max_sublayers_minus1);

  /// Écrit PTL dans un buffer
  /// @param writer Buffer writer pour écrire les données
  /// @param max_sublayers_minus1 Nombre maximum de sublayers - 1
  void Write(BufferWriter* writer, uint8_t max_sublayers_minus1) const;

  /// Retourne la taille en octets du PTL
  /// @param max_sublayers_minus1 Nombre maximum de sublayers - 1
  /// @return Taille en octets
  size_t ComputeSize(uint8_t max_sublayers_minus1) const;
};

/// Structure pour un tableau de NAL units
struct VvcNalArray {
  uint8_t array_completeness = 0;  // 1 bit
  uint8_t nal_unit_type = 0;       // 5 bits
  std::vector<std::vector<uint8_t>> nal_units;
};

/// Classe représentant VvcDecoderConfigurationRecord (vvcC box)
/// Selon ISO/IEC 14496-15:2022 Section 8.3.3.1 et ITU-T H.266
class VvcDecoderConfigurationRecord : public DecoderConfigurationRecord {
 public:
  VvcDecoderConfigurationRecord();
  ~VvcDecoderConfigurationRecord() override;

  /// Parse depuis des NAL units brutes (format Annex B)
  /// @param nal_units_data Buffer contenant les NAL units en format Annex B
  /// @return true si le parsing a réussi
  bool ParseFromNalUnits(const std::vector<uint8_t>& nal_units_data);

  /// Extrait le codec string pour DASH/HLS
  /// Format: vvc1.PROFILE.TIER_LEVEL[.CONSTRAINTS]
  /// Exemples: "vvc1.1.L153", "vvc1.1.H93", "vvi1.2.L156"
  /// @param codec_fourcc FourCC du codec (FOURCC_vvc1 ou FOURCC_vvi1)
  /// @return Codec string formaté
  std::string GetCodecString(FourCC codec_fourcc) const;

  /// Obtient les dimensions vidéo depuis le SPS
  /// @param width Largeur en sortie
  /// @param height Hauteur en sortie
  /// @return true si les dimensions sont disponibles
  bool GetVideoDimensions(uint32_t* width, uint32_t* height) const;

  /// Obtient le chroma subsampling depuis le SPS
  /// @return Format de chroma subsampling
  //VideoStreamInfo::ChromaSubsampling GetChromaSubsampling() const;
  VvcChromaSubsampling GetChromaSubsampling() const;

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

  /// Ajoute un NAL unit à la configuration
  /// @param nal_unit_type Type de NAL unit (VPS, SPS, PPS, etc.)
  /// @param data Données du NAL unit
  /// @param size Taille des données
  /// @param array_completeness Flag indiquant si l'array est complet
  void AddNalUnit(uint8_t nal_unit_type,
                  const uint8_t* data,
                  size_t size,
                  bool array_completeness = false);

 private:
  bool ParseInternal() override;

  // Parse les parameter sets pour extraire les informations
  bool ParseParameterSets();

  // Parse un SPS VVC (ITU-T H.266 Section 7.3.2.2)
  bool ParseSPS(const uint8_t* data, size_t size);

  // Parse un VPS VVC (ITU-T H.266 Section 7.3.2.1)
  bool ParseVPS(const uint8_t* data, size_t size);

  // Configuration fields selon ISO/IEC 14496-15:2022 Section 8.3.3.1
  uint8_t length_size_minus_one_ = 3;  // Typiquement 3 (4 bytes)
  bool ptl_present_flag_ = true;

  // Si ptl_present_flag == 1
  uint16_t ols_idx_ = 0;                // Output Layer Set index (9 bits)
  uint8_t num_sublayers_ = 0;           // Nombre de temporal sublayers (3 bits)
  uint8_t constant_frame_rate_ = 0;     // Constant frame rate flag (2 bits)
  uint8_t chroma_format_idc_ = 1;       // 4:2:0 par défaut (2 bits)
  uint8_t bit_depth_minus8_ = 0;        // 8 bits par défaut (3 bits)

  VvcPTLRecord ptl_record_;

  // Picture dimensions
  uint16_t max_picture_width_ = 0;
  uint16_t max_picture_height_ = 0;
  uint16_t avg_frame_rate_ = 0;

  // Arrays de NAL units (VPS, SPS, PPS, APS, etc.)
  std::vector<VvcNalArray> arrays_;

  // Données brutes des parameter sets pour parsing
  std::vector<uint8_t> vps_data_;
  std::vector<uint8_t> sps_data_;
  std::vector<uint8_t> pps_data_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_VVC_DECODER_CONFIGURATION_RECORD_H_