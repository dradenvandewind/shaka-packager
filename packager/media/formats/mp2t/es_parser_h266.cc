// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp2t/es_parser_h266.h>

#include <cstdint>

#include <algorithm>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/media/base/media_sample.h>
#include <packager/media/base/timestamp.h>

#include <packager/media/base/offset_byte_queue.h>
#include <packager/media/base/timestamp.h>
#include <packager/media/base/video_stream_info.h>

#include <packager/media/codecs/h266_byte_to_unit_stream_converter.h>
#include <packager/media/codecs/vvc_decoder_configuration_record.h>
#include <packager/media/codecs/h266_parser.h>
#include <packager/media/formats/mp2t/mp2t_common.h>



namespace shaka {
namespace media {
namespace mp2t {

namespace {

constexpr int kH266StartCodeSize = 2;

}  // namespace
/* 
EsParserH266::EsParserH266(uint32_t pid,
                           const NewStreamInfoCB& new_stream_info_cb,
                           const EmitSampleCB& emit_sample_cb
                           )
    : EsParserH26x(Nalu::kH266,
                   kH266StartCodeSize,
                   pid,
                   new_stream_info_cb,
                   emit_sample_cb),
      new_stream_info_cb_(new_stream_info_cb),
      decoder_config_check_pending_(false),
      parser_(new H266Parser()) {}
                   //sbr_in_mimetype */
EsParserH266::EsParserH266(uint32_t pid,
                           const NewStreamInfoCB& new_stream_info_cb,
                           const EmitSampleCB& emit_sample_cb)
    : EsParserH26x(Nalu::kH266,
                   std::make_unique<H266ByteToUnitStreamConverter>(),
                   pid,
                   emit_sample_cb),
      new_stream_info_cb_(new_stream_info_cb),
      decoder_config_check_pending_(false),
      parser_(new H266Parser()) {}

EsParserH266::~EsParserH266() {}

bool EsParserH266::ProcessNalu(const Nalu& nalu,
                               VideoSliceInfo* video_slice_info) {
  DCHECK(nalu.data());
  DCHECK(video_slice_info);

  const int nalu_type = nalu.type();

  switch (nalu_type) {
    case Nalu::H266_TRAIL_NUT:
    case Nalu::H266_STSA_NUT:
    case Nalu::H266_RADL_NUT:
    case Nalu::H266_RASL_NUT:
    case Nalu::H266_IDR_W_RADL:
    case Nalu::H266_IDR_N_LP:
    case Nalu::H266_CRA_NUT:
    case Nalu::H266_GDR_NUT:
      // VCL NALUs
      ProcessVclNalu(nalu, video_slice_info);
      break;

    case Nalu::H266_VPS_NUT:
    case Nalu::H266_SPS_NUT:
    case Nalu::H266_PPS_NUT:
    case Nalu::H266_PREFIX_APS_NUT:
    case Nalu::H266_SUFFIX_APS_NUT:
    case Nalu::H266_PH_NUT:
    case Nalu::H266_AUD_NUT:
    case Nalu::H266_EOS_NUT:
    case Nalu::H266_EOB_NUT:
    case Nalu::H266_PREFIX_SEI_NUT:
    case Nalu::H266_SUFFIX_SEI_NUT:
    case Nalu::H266_FD_NUT:
    case Nalu::H266_DCI_NUT:
    case Nalu::H266_OPI_NUT:
      // Non-VCL NALUs
      ProcessOtherNonVclNalu(nalu);
      break;

    default:
      // Reserved and unspecified NALUs
      DVLOG(1) << "Unhandled NALU type: " << nalu_type;
      break;
  }

  previous_nalu_type_ = nalu_type;
  return true;
}

bool EsParserH266::ProcessVclNalu(const Nalu& nalu,
                                  VideoSliceInfo* video_slice_info) {

  const bool is_key_frame = (nalu.type() == Nalu::H266_IDR_W_RADL ||
                               nalu.type() == Nalu::H266_IDR_N_LP);
  DVLOG(1) << "Nalu: slice KeyFrame=" << is_key_frame;


  // Parse slice header to get PPS ID and other information
  H266SliceHeader slice_header;
  auto status = (parser_->ParseSliceHeader(nalu, &slice_header));

  if ( status == H266Parser::kOk) {
    video_slice_info->valid = true;
    video_slice_info->is_key_frame = is_key_frame;
    video_slice_info->frame_num = 0; // frame_num is only for H264.
    video_slice_info->pps_id = slice_header.pic_parameter_set_id;
  } else if (status == H266Parser::kUnsupportedStream) {
    DVLOG(1) << "Unsupported feature in H.266 slice header.";
    new_stream_info_cb_(nullptr);  // Signal an error.
   
  } else {
          if(last_video_decoder_config_){
            return false;
          }
            
  }
  // nor sure to add additionnal code here 
  return true;

}

void EsParserH266::ProcessOtherNonVclNalu(const Nalu& nalu) {
  switch (nalu.type()) {
    case Nalu::H266_VPS_NUT: {
      int vps_id;
      if (parser_->ParseVps(nalu, &vps_id) == H266Parser::kOk) {
        // VPS parsed successfully
      }
      break;
    }
    case Nalu::H266_SPS_NUT: {
      int sps_id;
      if (parser_->ParseSps(nalu, &sps_id) == H266Parser::kOk) {
        // SPS parsed successfully
      }
      break;
    }
    case Nalu::H266_PPS_NUT: {
      int pps_id;
      if (parser_->ParsePps(nalu, &pps_id) == H266Parser::kOk) {
        // PPS parsed successfully
      }
      break;
    }
    case Nalu::H266_DCI_NUT:
      // Decoding Capability Information
      break;
    case Nalu::H266_OPI_NUT:
      // Operating Point Information
      break;
    case Nalu::H266_PREFIX_SEI_NUT:
    case Nalu::H266_SUFFIX_SEI_NUT:
      // SEI messages
      break;
    default:
      // Other NALUs
      break;
  }
}

bool EsParserH266::UpdateVideoDecoderConfig(int pps_id) {
  const H266Pps* pps = parser_->GetPps(pps_id);
  const H266Sps* sps;
  if (!pps) {
    return false;
  }

  sps = parser_->GetSps(pps->seq_parameter_set_id);
  if (!sps) {
    return false;
  }

  // Get parameter sets from parser
  std::vector<uint8_t> vps_data;
  std::vector<uint8_t> sps_data;
  std::vector<uint8_t> pps_data;
  
  std::vector<uint8_t> decoder_config_record;
  // Create decoder configuration record
  VvcDecoderConfigurationRecord decoder_config;

  if (!stream_converter()->GetDecoderConfigurationRecord(&decoder_config_record) || !decoder_config.Parse(decoder_config_record)) {
        DVLOG(1) << "Failure to construct an VccDecoderConfigurationRecord";
    return false;
  }

  if (last_video_decoder_config_) {
    // Check if the configuration has changed
    if (last_video_decoder_config_->codec_config() != decoder_config_record){

      LOG(WARNING) << "H.265 decoder configuration has changed.";
      last_video_decoder_config_->set_codec_config(decoder_config_record);
      
    }
    return true;
  }
  uint32_t coded_width = 0;
  uint32_t coded_height = 0;
  uint32_t pixel_width = 0;
  uint32_t pixel_height = 0;
  if(!ExtractResolutionFromSps(*sps, &coded_width, &coded_height,
                               &pixel_width, &pixel_height)) {
    DVLOG(1) << "Failed to extract video resolution from SPS.";
    return false;
  }

  const uint8_t nalu_length_size =
      H26xByteToUnitStreamConverter::kUnitStreamNaluLengthSize;
  const H26xStreamFormat stream_format = stream_converter()->stream_format();
  const FourCC codec_fourcc =
      stream_format == H26xStreamFormat::kNalUnitStreamWithParameterSetNalus
          ? FOURCC_vvc1
          : FOURCC_vvi1;
  last_video_decoder_config_ = std::make_shared<VideoStreamInfo>(
      pid(), kMpeg2Timescale, kInfiniteDuration, kCodecVVC, stream_format,
      decoder_config.GetCodecString(codec_fourcc), decoder_config_record.data(),
      decoder_config_record.size(), coded_width, coded_height, pixel_width,
      pixel_height, sps->vui_parameters.color_primaries,
      sps->vui_parameters.matrix_coefficients,
      sps->vui_parameters.transfer_characteristics, 0, nalu_length_size,
      std::string(), false);

  // Video config notification.
  new_stream_info_cb_(last_video_decoder_config_);


  return true;
}


}  // namespace mp2t
}  // namespace media
}  // namespace shaka