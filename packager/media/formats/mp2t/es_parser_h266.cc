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


namespace shaka {
namespace media {
namespace mp2t {

namespace {

constexpr int kH266StartCodeSize = 2;

}  // namespace

EsParserH266::EsParserH266(uint32_t pid,
                           const NewStreamInfoCB& new_stream_info_cb,
                           const EmitSampleCB& emit_sample_cb,
                           )
    : EsParserH26x(Nalu::kH266,
                   kH266StartCodeSize,
                   pid,
                   new_stream_info_cb,
                   emit_sample_cb) {}
                   //sbr_in_mimetype

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
      VLOG(1) << "Unhandled NALU type: " << nalu_type;
      break;
  }

  previous_nalu_type_ = nalu_type;
  return true;
}

void EsParserH266::ProcessVclNalu(const Nalu& nalu,
                                  VideoSliceInfo* video_slice_info) {
  // Parse slice header to get PPS ID and other information
  H266SliceHeader slice_header;
  if (parser_.ParseSliceHeader(nalu, &slice_header) != H266Parser::kOk) {
    return;
  }

  // Update video slice information
  video_slice_info->pps_id = slice_header.pps_id;
  video_slice_info->frame_num = slice_header.slice_pic_order_cnt_lsb;
  video_slice_info->idr_pic = (nalu.type() == Nalu::H266_IDR_W_RADL ||
                               nalu.type() == Nalu::H266_IDR_N_LP);
  video_slice_info->nal_ref_idc = nalu.nuh_layer_id(); // Use layer ID as reference indicator

  // Update decoder configuration if needed
  //if (video_slice_info->pps_id >= 0) {
  //  UpdateVideoDecoderConfig(video_slice_info->pps_id);
  //}
}

void EsParserH266::ProcessOtherNonVclNalu(const Nalu& nalu) {
  switch (nalu.type()) {
    case Nalu::H266_VPS_NUT: {
      int vps_id;
      if (parser_.ParseVps(nalu, &vps_id) == H266Parser::kOk) {
        // VPS parsed successfully
      }
      break;
    }
    case Nalu::H266_SPS_NUT: {
      int sps_id;
      if (parser_.ParseSps(nalu, &sps_id) == H266Parser::kOk) {
        // SPS parsed successfully
      }
      break;
    }
    case Nalu::H266_PPS_NUT: {
      int pps_id;
      if (parser_.ParsePps(nalu, &pps_id) == H266Parser::kOk) {
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
  const H266Pps* pps = parser_.GetPps(pps_id);
  if (!pps) {
    return false;
  }

  const H266Sps* sps = parser_.GetSps(pps->sps_id);
  if (!sps) {
    return false;
  }

  // Get parameter sets from parser
  std::vector<uint8_t> vps_data;
  std::vector<uint8_t> sps_data;
  std::vector<uint8_t> pps_data;
  
  std::vector<uint8_t> config_data;
  // Create decoder configuration record
  VvcDecoderConfigurationRecord decoder_config;
  //if (!decoder_config.Parse(sps_data, pps_data, vps_data)) {
  //  return false;
  //}
  if (!decoder_config.Parse(config_data)) {  // Use single parameter
    return false;
  }


  // Update stream info
  const int64_t kTimescale = 90000;
  const FourCC kCodecFourcc = FOURCC_vvc1; // Use appropriate FourCC for H.266

  std::string codec_string = decoder_config.GetCodecString(kCodecFourcc);
  
  // Create video stream info
  std::shared_ptr<VideoStreamInfo> video_stream_info(new VideoStreamInfo(
      pid(), kTimescale, kInfiniteDuration, kCodecVVC, codec_string,
      decoder_config.DecoderConfigurationRecord(), 0, sps->pic_width_max_in_luma_samples,
      sps->pic_height_max_in_luma_samples, 0, 1, sps->bit_depth_luma_minus8 + 8,
      sps->chroma_format_idc, nullptr, false));

  return EsParserH26x::UpdateVideoStreamInfo(video_stream_info);
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka