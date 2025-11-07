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
    : H26xByteToUnitStreamConverter(Nalu::kH266) {}

H266ByteToUnitStreamConverter::H266ByteToUnitStreamConverter(
    H26xStreamFormat stream_format)
    : H26xByteToUnitStreamConverter(Nalu::kH266, stream_format) {}

H266ByteToUnitStreamConverter::~H266ByteToUnitStreamConverter() {}

bool H266ByteToUnitStreamConverter::GetDecoderConfigurationRecord(
    std::vector<uint8_t>* decoder_config) const {
  DCHECK(decoder_config);

  if (last_sps_.empty() || last_pps_.empty() || last_vps_.empty()) {
    // No data available to construct VvcDecoderConfigurationRecord.
    return false;
  }

  // We need to parse the SPS to get the data to add to the record.
  int id;
  Nalu nalu;
  H266Parser parser;
  RCHECK(nalu.Initialize(Nalu::kH266, last_sps_.data(), last_sps_.size()));
  RCHECK(parser.ParseSps(nalu, &id) == H266Parser::kOk);
  const H266Sps* sps = parser.GetSps(id);
  RCHECK(parser.ParseVps(nalu, &id) == H266Parser::kOk);
  const H266Vps* vps = parser.GetVps(id);
  


  // Construct an VvcDecoderConfigurationRecord containing a single SPS, PPS,
  // and VPS NALU. Please refer to ISO/IEC 14496-15 for format specifics.
  BufferWriter buffer(last_sps_.size() + last_pps_.size() + last_vps_.size() +
                      100);
  
  // VVC decoder configuration record structure based on H.266 and MP4
  buffer.AppendInt(static_cast<uint8_t>(1) /* version */);
  buffer.AppendInt(static_cast<uint8_t>(0) /* flags */);
  
  // General profile, tier and level information
  // vvc_config_general_profile_idc, general_tier_flag, etc.
  //buffer.AppendInt(static_cast<uint8_t>(vps->profile_tier_level));
  //buffer.AppendInt(static_cast<uint8_t>(vps->general_profile_tier_level_data);
uint8_t general_profile_space = 0x00; // Adjust based on actual data
uint8_t general_tier_flag = 0x00;     // Adjust based on actual data
uint8_t general_profile_idc = static_cast<uint8_t>(vps->profile_tier_level.general_profile_idc);

// warning not sure check in ITU
buffer.AppendInt(static_cast<uint8_t>((general_profile_space << 6) | (general_tier_flag << 5) | general_profile_idc));

  
  // Bit depth and chroma format
  uint8_t bit_depth_info = ((sps->bit_depth_luma_minus8 & 0x07) << 4) | 
                           ((sps->bit_depth_chroma_minus8 & 0x07) << 1) |
                           (sps->chroma_format_idc & 0x03);
  buffer.AppendInt(bit_depth_info);
  
  // Average frame rate (0 for variable frame rate)
  buffer.AppendInt(static_cast<uint16_t>(0));
  
  // NALU length size
  buffer.AppendInt(static_cast<uint8_t>(kUnitStreamNaluLengthSize - 1));
  
  // Number of parameter sets
  buffer.AppendInt(static_cast<uint8_t>(3) /* numOfArrays */);

  // More parameter set NALUs may follow when strip_parameter_set_nalus is
  // disabled.
  const uint8_t array_completeness = strip_parameter_set_nalus() ? 0x80 : 0;

  // VPS (Video Parameter Set)
  buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_VPS_NUT));
  buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
  buffer.AppendInt(static_cast<uint16_t>(last_vps_.size()));
  buffer.AppendVector(last_vps_);

  // SPS (Sequence Parameter Set)
  buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_SPS_NUT));
  buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
  buffer.AppendInt(static_cast<uint16_t>(last_sps_.size()));
  buffer.AppendVector(last_sps_);

  // PPS (Picture Parameter Set)
  buffer.AppendInt(static_cast<uint8_t>(array_completeness | Nalu::H266_PPS_NUT));
  buffer.AppendInt(static_cast<uint16_t>(1) /* numNalus */);
  buffer.AppendInt(static_cast<uint16_t>(last_pps_.size()));
  buffer.AppendVector(last_pps_);

  buffer.SwapBuffer(decoder_config);
  return true;
}

bool H266ByteToUnitStreamConverter::ProcessNalu(const Nalu& nalu) {
  DCHECK(nalu.data());

  // Skip the start code, but keep the 2-byte NALU header.
  const uint8_t* nalu_ptr = nalu.data();
  const uint64_t nalu_size = nalu.payload_size() + nalu.header_size();

  switch (nalu.type()) {
    case Nalu::H266_SPS_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_sps_);
      // Grab SPS NALU.
      last_sps_.assign(nalu_ptr, nalu_ptr + nalu_size);
      return strip_parameter_set_nalus();
    case Nalu::H266_PPS_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_pps_);
      // Grab PPS NALU.
      last_pps_.assign(nalu_ptr, nalu_ptr + nalu_size);
      return strip_parameter_set_nalus();
    case Nalu::H266_VPS_NUT:
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_vps_);
      // Grab VPS NALU.
      last_vps_.assign(nalu_ptr, nalu_ptr + nalu_size);
      return strip_parameter_set_nalus();
    case Nalu::H266_DCI_NUT:
      // DCI (Decoding Capability Information) - optional in VVC
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_dci_);
      last_dci_.assign(nalu_ptr, nalu_ptr + nalu_size);
      return strip_parameter_set_nalus();
    case Nalu::H266_OPI_NUT:
      // OPI (Operating Point Information) - optional in VVC
      if (strip_parameter_set_nalus())
        WarnIfNotMatch(nalu.type(), nalu_ptr, nalu_size, last_opi_);
      last_opi_.assign(nalu_ptr, nalu_ptr + nalu_size);
      return strip_parameter_set_nalus();
    case Nalu::H266_AUD_NUT:
      // Ignore AUD NALU.
      return true;
    case Nalu::H266_PH_NUT:
      // Picture Header - can be stripped if not needed
      return strip_parameter_set_nalus();
    case Nalu::H266_PREFIX_SEI_NUT:
    case Nalu::H266_SUFFIX_SEI_NUT:
      // SEI NALUs - can be stripped if not needed
      return strip_parameter_set_nalus();
    default:
      // Have the base class handle other NALU types.
      return false;
  }
}

}  // namespace media
}  // namespace shaka