// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/codecs/h266_byte_to_unit_stream_converter.h>

#include <algorithm>
#include <vector>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/media/codecs/vvc_decoder_configuration_record.h>

namespace shaka {
namespace media {

namespace {

constexpr size_t kNumNaluLengthBytes = 4;

}  // namespace

H266ByteToUnitStreamConverter::H266ByteToUnitStreamConverter()
    : H26xByteToUnitStreamConverter() {}

H266ByteToUnitStreamConverter::H266ByteToUnitStreamConverter(
    H26xStreamFormat stream_format)
    : H26xByteToUnitStreamConverter(stream_format) {}

H266ByteToUnitStreamConverter::~H266ByteToUnitStreamConverter() {}

bool H266ByteToUnitStreamConverter::GetDecoderConfigurationRecord(
    std::vector<uint8_t>* decoder_config) const {
  DCHECK(decoder_config);

  // For H.266, we need at least VPS and SPS to create a valid configuration
  if (last_vps_.empty() || last_sps_.empty()) {
    LOG(ERROR) << "VPS or SPS not available for creating decoder configuration";
    return false;
  }

  VVCDecoderConfigurationRecord vvc_config;
  
  // Add VPS
  if (!last_vps_.empty()) {
    vvc_config.AddNalu(last_vps_);
  }

  // Add SPS
  if (!last_sps_.empty()) {
    vvc_config.AddNalu(last_sps_);
  }

  // Add PPS if available
  if (!last_pps_.empty()) {
    vvc_config.AddNalu(last_pps_);
  }

  // Add DCI if available (H.266 specific)
  if (!last_dci_.empty()) {
    vvc_config.AddNalu(last_dci_);
  }

  // Add OPI if available (H.266 specific)
  if (!last_opi_.empty()) {
    vvc_config.AddNalu(last_opi_);
  }

  // Set necessary configuration parameters
  vvc_config.set_nalu_length_size(kNumNaluLengthBytes);

  return vvc_config.Write(decoder_config);
}

std::string H266ByteToUnitStreamConverter::GetCodecString(FourCC codec_fourcc) const {
  // For H.266, we need to extract profile/tier/level information from VPS/SPS
  // This is a simplified version - actual implementation would parse the VPS/SPS
  
  // Default values - should be extracted from parameter sets
  int general_profile_idc = 1;  // Main profile
  int general_tier_flag = 0;    // Main tier
  int general_level_idc = 60;   // Level 6.0
  
  char codec_string[128];
  
  if (codec_fourcc == FOURCC_vvc1 || codec_fourcc == FOURCC_vvi1) {
    // Format: vvc1.P.LL.T.TT where:
    // P = profile_idc, LL = level_idc, T = tier_flag, TT = sub_profile_idc
    snprintf(codec_string, sizeof(codec_string), "vvc1.%02x.%02x.%02x.%02x",
             general_profile_idc, general_level_idc, general_tier_flag, 0);
  } else {
    // Fallback for unknown FourCC
    snprintf(codec_string, sizeof(codec_string), "vvc1.%02x.%02x.%02x.%02x",
             general_profile_idc, general_level_idc, general_tier_flag, 0);
  }
  
  return std::string(codec_string);
}

bool H266ByteToUnitStreamConverter::ProcessNalu(const Nalu& nalu) {
  const uint8_t* nalu_data = nalu.data();
  const size_t nalu_size = nalu.data_size();

  switch (nalu.type()) {
    case Nalu::H266_VPS_NUT:
      last_vps_.assign(nalu_data, nalu_data + nalu_size);
      has_vps_ = true;
      DVLOG(3) << "Found VPS, size: " << nalu_size;
      break;

    case Nalu::H266_SPS_NUT:
      last_sps_.assign(nalu_data, nalu_data + nalu_size);
      has_sps_ = true;
      DVLOG(3) << "Found SPS, size: " << nalu_size;
      break;

    case Nalu::H266_PPS_NUT:
      last_pps_.assign(nalu_data, nalu_data + nalu_size);
      has_pps_ = true;
      DVLOG(3) << "Found PPS, size: " << nalu_size;
      break;

    case Nalu::H266_DCI_NUT:
      last_dci_.assign(nalu_data, nalu_data + nalu_size);
      DVLOG(3) << "Found DCI, size: " << nalu_size;
      break;

    case Nalu::H266_OPI_NUT:
      last_opi_.assign(nalu_data, nalu_data + nalu_size);
      DVLOG(3) << "Found OPI, size: " << nalu_size;
      break;

    case Nalu::H266_PREFIX_APS_NUT:
    case Nalu::H266_SUFFIX_APS_NUT:
      // APS are typically not included in decoder configuration record
      DVLOG(4) << "Found APS, type: " << nalu.type();
      break;

    case Nalu::H266_AUD_NUT:
      // Access Unit Delimiter - can be used for synchronization but not needed
      // in configuration
      DVLOG(4) << "Found AUD";
      break;

    default:
      if (nalu.is_vcl()) {
        DVLOG(4) << "Found VCL NALU, type: " << nalu.type();
      } else {
        DVLOG(4) << "Found other NALU, type: " << nalu.type();
      }
      break;
  }

  // For H.266, we need to include parameter sets in the output stream
  // based on the stream format setting
  if (stream_format() == H26xStreamFormat::kNalUnitStreamWithParameterSetNalus) {
    // Include parameter sets in the output stream
    switch (nalu.type()) {
      case Nalu::H266_VPS_NUT:
      case Nalu::H266_SPS_NUT:
      case Nalu::H266_PPS_NUT:
      case Nalu::H266_DCI_NUT:
      case Nalu::H266_OPI_NUT:
        // These will be included in the output stream
        return true;
      default:
        // Other NALUs are processed normally
        break;
    }
  } else {
    // In kNalUnitStream format, parameter sets are only in decoder configuration
    // Don't include them in the elementary stream
    switch (nalu.type()) {
      case Nalu::H266_VPS_NUT:
      case Nalu::H266_SPS_NUT:
      case Nalu::H266_PPS_NUT:
      case Nalu::H266_DCI_NUT:
      case Nalu::H266_OPI_NUT:
        return false;  // Don't include in elementary stream
      default:
        break;
    }
  }

  return true;
}

}  // namespace media
}  // namespace shaka