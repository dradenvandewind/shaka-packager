// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/codecs/h266_byte_to_unit_stream_converter.h>

#include <limits>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/media/base/buffer_writer.h>
#include <packager/media/base/rcheck.h>
#include <packager/media/codecs/h266_parser.h>

namespace shaka {
namespace media {

H266ByteToUnitStreamConverter::H266ByteToUnitStreamConverter()
    : H26xByteToUnitStreamConverter(Nalu::kVVC) {}

H266ByteToUnitStreamConverter::H266ByteToUnitStreamConverter(
    H26xStreamFormat stream_format)
    : H26xByteToUnitStreamConverter(Nalu::kVVC, stream_format) {}

H266ByteToUnitStreamConverter::~H266ByteToUnitStreamConverter() {}

bool H266ByteToUnitStreamConverter::GetDecoderConfigurationRecord(
    std::vector<uint8_t>* decoder_config) const {
  DCHECK(decoder_config);

  if (last_vps_.empty() || last_sps_.empty()) {
    // No data available to construct VVCDecoderConfigurationRecord.
    LOG(ERROR) << "VPS or SPS not available for VVC decoder configuration";
    return false;
  }

  // We need to parse the SPS to get the data to add to the record.
  int id;
  Nalu nalu;
  H266Parser parser;
  RCHECK(nalu.Initialize(Nalu::kVVC, last_sps_.data(), last_sps_.size()));
  RCHECK(parser.ParseSps(nalu, &id) == H266Parser::kOk);
  const H266Sps* sps = parser.GetSps(id);

  // Construct a VVCDecoderConfigurationRecord.
  // Format is similar to HEVC but with H.266 specific fields.
  BufferWriter buffer(last_vps_.size() + last_sps_.size() + last_pps_.size() +
                      last_dci_.size() + last_opi_.size() + 100);
  
  // Version and profile information
  buffer.AppendInt(static_cast<uint8_t>(1) /* version */);
  
  // General profile, tier and level data (12 bytes)
  for (int byte : sps->general_profile_tier_level_data)
    buffer.AppendInt(static_cast<uint8_t>(byte));

  // H.266 specific fields
  buffer.AppendInt(static_cast<uint8_t>(sps->vps_id));
  buffer.AppendInt(static_cast<uint8_t>(sps->max_sublayers_minus1));
  buffer.AppendInt(static_cast<uint8_t>(sps->chroma_format_idc));
  buffer.AppendInt(static_cast<uint8_t>(sps->bit_depth_luma_minus8));
  buffer.AppendInt(static_cast<uint8_t>(sps->bit_depth_chroma_minus8));
  
  // Frame rate information (simplified)
  buffer.AppendInt(static_cast<uint16_t>(0) /* avgFrameRate */);
  
  // NALU length size and number of arrays
  buffer.AppendInt(static_cast<uint8_t>(kUnitStreamNaluLengthSize - 1));
  
  // Count number of parameter set arrays to include
  int num_arrays = 2; // VPS and SPS are mandatory
  if (!last_pps_.empty()) num_arrays++;
  if (!last_dci_.empty()) num_arrays++;
  if (!last_opi_.empty()) num_arrays++;
  
  buffer.AppendInt(static_cast<uint8_t>(num_arrays));

  // The array_completeness flag indicates if all NALUs of this type are included
  const uint8_t array_completeness = strip_parameter_set_nalus() ? 0x80 : 0;

  // VPS (mandatory for H.266)
  buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_VPS_NUT));
  buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
  buffer.AppendInt(static_cast<uint16_t>(last_vps_.size()));
  buffer.AppendVector(last_vps_);

  // SPS (mandatory for H.266)
  buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_SPS_NUT));
  buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
  buffer.AppendInt(static_cast<uint16_t>(last_sps_.size()));
  buffer.AppendVector(last_sps_);

  // PPS (optional but usually present)
  if (!last_pps_.empty()) {
    buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_PPS_NUT));
    buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
    buffer.AppendInt(static_cast<uint16_t>(last_pps_.size()));
    buffer.AppendVector(last_pps_);
  }

  // DCI (H.266 specific, optional)
  if (!last_dci_.empty()) {
    buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_DCI_NUT));
    buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
    buffer.AppendInt(static_cast<uint16_t>(last_dci_.size()));
    buffer.AppendVector(last_dci_);
  }

  // OPI (H.266 specific, optional)
  if (!last_opi_.empty()) {
    buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_OPI_NUT));
    buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
    buffer.AppendInt(static_cast<uint16_t>(last_opi_.size()));
    buffer.AppendVector(last_opi_);
  }

  buffer.SwapBuffer(decoder_config);
  return true;
}

std::string H266ByteToUnitStreamConverter::GetCodecString(FourCC codec_fourcc) const {
  // For H.266, we need to extract profile/tier/level information from VPS/SPS
  if (last_sps_.empty()) {
    LOG(WARNING) << "SPS not available for generating codec string";
    return "vvc1.00.3C.00.00"; // Default fallback
  }

  // Parse SPS to get profile/tier/level information
  int id;
  Nalu nalu;
  H266Parser parser;
  if (!nalu.Initialize(Nalu::kVVC, last_sps_.data(), last_sps_.size()) ||
      parser.ParseSps(nalu, &id) != H266Parser::kOk) {
    LOG(WARNING) << "Failed to parse SPS for codec string";
    return "vvc1.00.3C.00.00"; // Default fallback
  }

  const H266Sps* sps = parser.GetSps(id);
  if (!sps) {
    LOG(WARNING) << "Failed to get SPS for codec string";
    return "vvc1.00.3C.00.00"; // Default fallback
  }

  // Extract profile, tier, level from general_profile_tier_level_data
  // The structure is: 
  // byte 0: general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
  // byte 1-4: general_profile_compatibility_flags
  // byte 5-10: general_constraint_indicator_flags  
  // byte 11: general_level_idc
  
  int general_profile_idc = sps->general_profile_tier_level_data[0] & 0x1F;
  int general_tier_flag = (sps->general_profile_tier_level_data[0] >> 5) & 0x1;
  int general_level_idc = sps->general_profile_tier_level_data[11];

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
  DCHECK(nalu.data());

  // Skip the start code, but keep the NALU header.
  const uint8_t* nalu_ptr = nalu.data();
  const uint64_t nalu_size = nalu.payload_size() + nalu.header_size();

  switch (nalu.type()) {
    case Nalu::H266_VPS_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_vps_);
      // Grab VPS NALU.
      last_vps_.assign(nalu_ptr, nalu_ptr + nalu_size);
      has_vps_ = true;
      DVLOG(3) << "Found VPS, size: " << nalu_size;
      return strip_parameter_set_nalus();

    case Nalu::H266_SPS_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_sps_);
      // Grab SPS NALU.
      last_sps_.assign(nalu_ptr, nalu_ptr + nalu_size);
      has_sps_ = true;
      DVLOG(3) << "Found SPS, size: " << nalu_size;
      return strip_parameter_set_nalus();

    case Nalu::H266_PPS_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_pps_);
      // Grab PPS NALU.
      last_pps_.assign(nalu_ptr, nalu_ptr + nalu_size);
      has_pps_ = true;
      DVLOG(3) << "Found PPS, size: " << nalu_size;
      return strip_parameter_set_nalus();

    case Nalu::H266_DCI_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_dci_);
      // Grab DCI NALU.
      last_dci_.assign(nalu_ptr, nalu_ptr + nalu_size);
      DVLOG(3) << "Found DCI, size: " << nalu_size;
      return strip_parameter_set_nalus();

    case Nalu::H266_OPI_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_opi_);
      // Grab OPI NALU.
      last_opi_.assign(nalu_ptr, nalu_ptr + nalu_size);
      DVLOG(3) << "Found OPI, size: " << nalu_size;
      return strip_parameter_set_nalus();

    case Nalu::H266_AUD_NUT:
      // Ignore AUD NALU.
      DVLOG(4) << "Ignoring AUD NALU";
      return true;

    case Nalu::H266_PREFIX_APS_NUT:
    case Nalu::H266_SUFFIX_APS_NUT:
      // APS are typically not included in configuration records
      DVLOG(4) << "Ignoring APS NALU, type: " << nalu.type();
      return strip_parameter_set_nalus();

    default:
      if (nalu.is_vcl()) {
        DVLOG(4) << "Processing VCL NALU, type: " << nalu.type();
      } else {
        DVLOG(4) << "Processing other NALU, type: " << nalu.type();
      }
      // Have the base class handle other NALU types.
      return false;
  }
}

}  // namespace media
}  // namespace shaka