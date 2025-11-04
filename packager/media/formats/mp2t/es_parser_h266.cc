// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp2t/es_parser_h266.h>

#include <cstdint>

#include <absl/log/log.h>

#include <packager/macros/logging.h>
#include <packager/media/base/media_sample.h>
#include <packager/media/base/offset_byte_queue.h>
#include <packager/media/base/timestamp.h>
#include <packager/media/base/video_stream_info.h>
#include <packager/media/codecs/h266_byte_to_unit_stream_converter.h>
#include <packager/media/codecs/h266_parser.h>
#include <packager/media/codecs/vvc_decoder_configuration_record.h>
#include <packager/media/formats/mp2t/mp2t_common.h>

namespace shaka {
namespace media {
namespace mp2t {

EsParserH266::EsParserH266uint32_t pid,
                           const NewStreamInfoCB& new_stream_info_cb,
                           const EmitSampleCB& emit_sample_cb)
    : EsParserH26x(Nalu::kH266,
                   std::unique_ptr<H26xByteToUnitStreamConverter>(
                       new H266ByteToUnitStreamConverter()),
                   pid,
                   emit_sample_cb),
      new_stream_info_cb_(new_stream_info_cb),
      decoder_config_check_pending_(false),
      h266_parser_(new H266Parser()) {}

EsParserH266::~EsParserH266() {}

void EsParserH266::Reset() {
  DVLOG(1) << "EsParserH266::Reset";
  h266_parser_.reset(new H266Parser());
  last_video_decoder_config_ = std::shared_ptr<VideoStreamInfo>();
  decoder_config_check_pending_ = false;
  EsParserH26x::Reset();
}

bool EsParserH266::ProcessNalu(const Nalu& nalu,
                               VideoSliceInfo* video_slice_info) {
  video_slice_info->valid = false;
  switch (nalu.type()) {
    case Nalu::H266_AUD_NUT: {
      DVLOG(LOG_LEVEL_ES) << "Nalu: AUD";
      // Access Unit Delimiter - New acces unit
      // iN H.266, AUD can have a payload with pic_type
      if (nalu.payload_size() >= 1) {
        uint8_t pic_type = nalu.data()[nalu.header_size()] & 0x07;
        DVLOG(LOG_LEVEL_ES) << "AUD pic_type: " << static_cast<int>(pic_type);
      }
      break;
     }
     case Nalu::H266_SPS_NUT: {
      DVLOG(LOG_LEVEL_ES) << "Nalu: SPS";
      // Sequence Parameter Set - sequence parameter
      int sps_id;
      auto status = h266_parser_->ParseSps(nalu, &sps_id);
      if (status == H266Parser::kOk) {
        decoder_config_check_pending_ = true;
        DVLOG(LOG_LEVEL_ES) << "SPS parsed successfully, id: " << sps_id;
      } else if (status == H266Parser::kUnsupportedStream) {
        LOG(WARNING) << "Unsupported SPS stream";
        new_stream_info_cb_(nullptr);
      } else {
        LOG(ERROR) << "SPS parsing failed";
        return false;
      }
      break;
     }
     case Nalu::H266_PPS_NUT: {
      DVLOG(LOG_LEVEL_ES) << "Nalu: PPS";
      // Picture Parameter Set - paramètres de l'image
      int pps_id;
      auto status = h266_parser_->ParsePps(nalu, &pps_id);
      if (status == H266Parser::kOk) {
        decoder_config_check_pending_ = true;
        DVLOG(LOG_LEVEL_ES) << "PPS parsed successfully, id: " << pps_id;
      } else if (status == H266Parser::kUnsupportedStream) {
        LOG(WARNING) << "Unsupported PPS stream";
        new_stream_info_cb_(nullptr);
      } else {
        // En H.266, on peut tolérer l'échec du PPS si on n'a pas encore de config
        if (last_video_decoder_config_) {
          LOG(ERROR) << "PPS parsing failed with existing decoder config";
          return false;
        } else {
          LOG(WARNING) << "PPS parsing failed but no decoder config yet, continuing";
        }
      }
      break;
    }
    default: {
      // Tous les autres types NAL (VCL et non-VCL)
      if (nalu.is_vcl() && nalu.nuh_layer_id() == 0) {
        // NAL units VCL (Video Coding Layer) - données de slice
        ProcessVclNalu(nalu, video_slice_info);
      } else {
        // NAL units non-VCL autres que AUD, VPS, SPS, PPS
        ProcessOtherNonVclNalu(nalu);
      }
      break;
    }
  }

  return true;
}

void EsParserH266::ProcessVclNalu(const Nalu& nalu, VideoSliceInfo* video_slice_info) {
  // Determine if it is a keyframe
  const bool is_key_frame = nalu.type() == Nalu::H266_IDR_W_RADL ||
                            nalu.type() == Nalu::H266_IDR_N_LP ||
                            nalu.type() == Nalu::H266_CRA_NUT ||
                            nalu.type() == Nalu::H266_GDR_NUT;
  
  DVLOG(LOG_LEVEL_ES) << "Nalu: VCL slice Type=" << nalu.type() 
                     << " KeyFrame=" << is_key_frame
                     << " Layer=" << nalu.nuh_layer_id()
                     << " TemporalId=" << nalu.nuh_temporal_id();

  H266SliceHeader shdr;
  auto status = h266_parser_->ParseSliceHeader(nalu, &shdr);
  
  if (status == H266Parser::kOk) {
    video_slice_info->valid = true;
    video_slice_info->is_key_frame = is_key_frame;
    video_slice_info->frame_num = 0;  // H.266 n'utilise pas frame_num
    video_slice_info->pps_id = shdr.pic_parameter_set_id;
    video_slice_info->temporal_id = nalu.nuh_temporal_id();
    
    // For H.266, the POC can be used from the slice header.
    if (shdr.pic_order_cnt_lsb != -1) {
      video_slice_info->poc = shdr.pic_order_cnt_lsb;
    }
    
    DVLOG(LOG_LEVEL_ES) << "Slice parsed: PPS_id=" << shdr.pic_parameter_set_id
                       << " first_slice=" << shdr.first_slice_segment_in_pic_flag;
    
  } else if (status == H266Parser::kUnsupportedStream) {
    LOG(WARNING) << "Unsupported VCL stream";
    new_stream_info_cb_(nullptr);
  } else {
    // For slices, we can be more tolerant of parsing errors.
    if (last_video_decoder_config_) {
      LOG(ERROR) << "VCL NALU parsing failed with existing decoder config";
    } else {
      LOG(WARNING) << "VCL NALU parsing failed but no decoder config yet, continuing";
    }
  }
}

void EsParserH266::ProcessOtherNonVclNalu(const Nalu& nalu) {
  // Management of other non-VCL NAL types
  switch (nalu.type()) {
    case Nalu::H266_PREFIX_APS_NUT:
    case Nalu::H266_SUFFIX_APS_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: APS (ignored for now)";
      break;
      
    case Nalu::H266_PH_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: Picture Header (ignored for now)";
      break;
      
    case Nalu::H266_PREFIX_SEI_NUT:
    case Nalu::H266_SUFFIX_SEI_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: SEI (ignored for now)";
      break;
      
    case Nalu::H266_DCI_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: DCI (ignored for now)";
      break;
      
    case Nalu::H266_OPI_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: OPI (ignored for now)";
      break;
      
    case Nalu::H266_EOS_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: End of Sequence";
      break;
      
    case Nalu::H266_EOB_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: End of Bitstream";
      break;
      
    case Nalu::H266_FD_NUT:
      DVLOG(LOG_LEVEL_ES) << "Nalu: Filler Data";
      break;
      
    default:
      // Reserved or unspecified types
      if (nalu.type() >= Nalu::H266_RSV_VCL_4 && nalu.type() <= Nalu::H266_RSV_VCL_6) {
        DVLOG(LOG_LEVEL_ES) << "Nalu: Reserved VCL " << nalu.type();
      } else if (nalu.type() >= Nalu::H266_RSV_NVCL_26 && nalu.type() <= Nalu::H266_RSV_NVCL_27) {
        DVLOG(LOG_LEVEL_ES) << "Nalu: Reserved non-VCL " << nalu.type();
      } else if (nalu.type() >= Nalu::H266_UNSPEC_28 && nalu.type() <= Nalu::H266_UNSPEC_31) {
        DVLOG(LOG_LEVEL_ES) << "Nalu: Unspecified " << nalu.type();
      } else {
        DVLOG(LOG_LEVEL_ES) << "Nalu: Unknown type " << nalu.type();
      }
      break;
  }
  
  // For non-base layers, we log a warning.
  if (nalu.nuh_layer_id() != 0) {
    DVLOG(LOG_LEVEL_ES) << "Non-base layer NAL unit ignored: layer_id=" 
                       << nalu.nuh_layer_id();
  }
}



bool EsParserH266::UpdateVideoDecoderConfig(int pps_id) {
  // Update the video decoder configuration if needed.
  if (!decoder_config_check_pending_)
    return true;

  const H266Pps* pps = h266_parser_->GetPps(pps_id);
  const H266Sps* sps;
  const H266Vps* vps;
  
  if (!pps) {
    // Only accept an invalid PPS at the beginning when the stream
    // does not necessarily start with an SPS/PPS/IDR.
    // In this case, the initial frames are conveyed to the upper layer with
    // an invalid VideoDecoderConfig and it's up to the upper layer
    // to process this kind of frame accordingly.
    return last_video_decoder_config_ == nullptr;
  } else {
    sps = h266_parser_->GetSps(pps->seq_parameter_set_id);
    if (!sps)
      return false;
    
    vps = h266_parser_->GetVps(sps->vps_id);
    if (!vps)
      return false;
      
    decoder_config_check_pending_ = false;
  }

  std::vector<uint8_t> decoder_config_record;
  VVCDecoderConfigurationRecord decoder_config;
  if (!stream_converter()->GetDecoderConfigurationRecord(
          &decoder_config_record) ||
      !decoder_config.Parse(decoder_config_record)) {
    DLOG(ERROR) << "Failure to construct an VVCDecoderConfigurationRecord";
    return false;
  }

  if (last_video_decoder_config_) {
    if (last_video_decoder_config_->codec_config() != decoder_config_record) {
      // Video configuration has changed. Issue warning.
      // TODO(tinskip): Check the nature of the configuration change. Only
      // minor configuration changes (such as frame ordering) can be handled
      // gracefully by decoders without notification. Major changes (such as
      // video resolution changes) should be treated as errors.
      LOG(WARNING) << "H.266 decoder configuration has changed.";
      last_video_decoder_config_->set_codec_config(decoder_config_record);
    }
    return true;
  }

  uint32_t coded_width = 0;
  uint32_t coded_height = 0;
  uint32_t pixel_width = 1;
  uint32_t pixel_height = 1;
  
  // Extract resolution from SPS for H.266
  if (!ExtractResolutionFromSps(*sps, &coded_width, &coded_height, &pixel_width,
                                &pixel_height)) {
    LOG(ERROR) << "Failed to parse SPS for resolution.";
    return false;
  }

  const uint8_t nalu_length_size =
      H26xByteToUnitStreamConverter::kUnitStreamNaluLengthSize;
  const H26xStreamFormat stream_format = stream_converter()->stream_format();
  
  // H.266 uses different FourCC codes
  const FourCC codec_fourcc =
      stream_format == H26xStreamFormat::kNalUnitStreamWithParameterSetNalus
          ? FOURCC_vvi1  // 'vvi1' for VVC in MP4 with parameter sets
          : FOURCC_vvc1; // 'vvc1' for VVC in MP4

  // Extract color information from SPS VUI if available
  uint32_t color_primaries = 0;
  uint32_t matrix_coefficients = 0;
  uint32_t transfer_characteristics = 0;
  
  if (sps->vui_parameters_present && sps->vui_parameters.vui_color_description_present_flag) {
    color_primaries = sps->vui_parameters.color_primaries;
    matrix_coefficients = sps->vui_parameters.matrix_coefficients;
    transfer_characteristics = sps->vui_parameters.transfer_characteristics;
  }

  // Create codec string with profile, tier, and level
  std::string codec_string = decoder_config.GetCodecString(codec_fourcc);
  
  last_video_decoder_config_ = std::make_shared<VideoStreamInfo>(
      pid(), kMpeg2Timescale, kInfiniteDuration, 
      kCodecH266,  // New codec type for H.266
      stream_format,
      codec_string, 
      decoder_config_record.data(),
      decoder_config_record.size(), 
      coded_width, coded_height, 
      pixel_width, pixel_height, 
      color_primaries,
      matrix_coefficients,
      transfer_characteristics, 
      0,  // trick_play_factor
      nalu_length_size,
      std::string(),  // language
      false);         // seek_preroll

  // Video config notification.
  new_stream_info_cb_(last_video_decoder_config_);

  DVLOG(1) << "H.266 decoder config updated: " << coded_width << "x" << coded_height
           << " codec: " << codec_string;

  return true;
}

bool EsParserH266::UpdateVideoDecoderConfig(int pps_id) {
  // Update the video decoder configuration if needed.
  if (!decoder_config_check_pending_)
    return true;

  const H266Pps* pps = h266_parser_->GetPps(pps_id);
  const H266Sps* sps;
  const H266Vps* vps;
  
  if (!pps) {
    // Only accept an invalid PPS at the beginning when the stream
    // does not necessarily start with an SPS/PPS/IDR.
    // In this case, the initial frames are conveyed to the upper layer with
    // an invalid VideoDecoderConfig and it's up to the upper layer
    // to process this kind of frame accordingly.
    return last_video_decoder_config_ == nullptr;
  } else {
    sps = h266_parser_->GetSps(pps->seq_parameter_set_id);
    if (!sps)
      return false;
    
    vps = h266_parser_->GetVps(sps->vps_id);
    if (!vps)
      return false;
      
    decoder_config_check_pending_ = false;
  }

  std::vector<uint8_t> decoder_config_record;
  VVCDecoderConfigurationRecord decoder_config;
  if (!stream_converter()->GetDecoderConfigurationRecord(
          &decoder_config_record) ||
      !decoder_config.Parse(decoder_config_record)) {
    DLOG(ERROR) << "Failure to construct an VVCDecoderConfigurationRecord";
    return false;
  }

  if (last_video_decoder_config_) {
    if (last_video_decoder_config_->codec_config() != decoder_config_record) {
      // Video configuration has changed. Issue warning.
      // TODO(tinskip): Check the nature of the configuration change. Only
      // minor configuration changes (such as frame ordering) can be handled
      // gracefully by decoders without notification. Major changes (such as
      // video resolution changes) should be treated as errors.
      LOG(WARNING) << "H.266 decoder configuration has changed.";
      last_video_decoder_config_->set_codec_config(decoder_config_record);
    }
    return true;
  }

  uint32_t coded_width = 0;
  uint32_t coded_height = 0;
  uint32_t pixel_width = 1;
  uint32_t pixel_height = 1;
  
  // Extract resolution from SPS for H.266
  if (!ExtractResolutionFromSps(*sps, &coded_width, &coded_height, &pixel_width,
                                &pixel_height)) {
    LOG(ERROR) << "Failed to parse SPS for resolution.";
    return false;
  }

  const uint8_t nalu_length_size =
      H26xByteToUnitStreamConverter::kUnitStreamNaluLengthSize;
  const H26xStreamFormat stream_format = stream_converter()->stream_format();
  
  // H.266 uses different FourCC codes
  const FourCC codec_fourcc =
      stream_format == H26xStreamFormat::kNalUnitStreamWithParameterSetNalus
          ? FOURCC_vvi1  // 'vvi1' for VVC in MP4 with parameter sets
          : FOURCC_vvc1; // 'vvc1' for VVC in MP4

  // Extract color information from SPS VUI if available
  uint32_t color_primaries = 0;
  uint32_t matrix_coefficients = 0;
  uint32_t transfer_characteristics = 0;
  
  if (sps->vui_parameters_present && sps->vui_parameters.vui_color_description_present_flag) {
    color_primaries = sps->vui_parameters.color_primaries;
    matrix_coefficients = sps->vui_parameters.matrix_coefficients;
    transfer_characteristics = sps->vui_parameters.transfer_characteristics;
  }

  // Create codec string with profile, tier, and level
  std::string codec_string = decoder_config.GetCodecString(codec_fourcc);
  
  last_video_decoder_config_ = std::make_shared<VideoStreamInfo>(
      pid(), kMpeg2Timescale, kInfiniteDuration, 
      kCodecH266,  // New codec type for H.266
      stream_format,
      codec_string, 
      decoder_config_record.data(),
      decoder_config_record.size(), 
      coded_width, coded_height, 
      pixel_width, pixel_height, 
      color_primaries,
      matrix_coefficients,
      transfer_characteristics, 
      0,  // trick_play_factor
      nalu_length_size,
      std::string(),  // language
      false);         // seek_preroll

  // Video config notification.
  new_stream_info_cb_(last_video_decoder_config_);

  DVLOG(1) << "H.266 decoder config updated: " << coded_width << "x" << coded_height
           << " codec: " << codec_string;

  return true;
}

int64_t EsParserH266::CalculateSampleDuration(int pps_id) {
  auto pps = h266_parser_->GetPps(pps_id);
  if (pps) {
    auto sps_id = pps->seq_parameter_set_id;
    auto sps = h266_parser_->GetSps(sps_id);
    if (sps && sps->vui_parameters_present &&
        sps->vui_parameters.vui_timing_info_present_flag) {
      // H.266 VUI timing info structure is similar to H.265
      return static_cast<int64_t>(kMpeg2Timescale) *
             sps->vui_parameters.vui_num_units_in_tick * 2 /
             sps->vui_parameters.vui_time_scale;
    }
    
    // Also check VPS for timing information
    if (sps) {
      auto vps_id = sps->vps_id;
      auto vps = h266_parser_->GetVps(vps_id);
      if (vps && vps->vps_timing_info_present_flag) {
        return static_cast<int64_t>(kMpeg2Timescale) *
               vps->vps_num_units_in_tick * 2 /
               vps->vps_time_scale;
      }
    }
  }
  
  LOG(WARNING) << "[MPEG-2 TS] PID " << pid()
               << " Cannot calculate frame rate from SPS/VPS.";
  
  // Try to use default timing or fallback to arbitrary safe duration
  if (last_video_decoder_config_) {
    // Use a reasonable default based on typical frame rates
    return 0.033 * kMpeg2Timescale;  // 33ms ≈ 30fps
  }
  
  return 0.001 * kMpeg2Timescale;  // 1ms fallback
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka


/*
 private:
  // Extract resolution from H.266 SPS
  bool ExtractResolutionFromSps(const H266Sps& sps,
                               uint32_t* coded_width,
                               uint32_t* coded_height,
                               uint32_t* pixel_width,
                               uint32_t* pixel_height) {
    // H.266 uses different syntax for resolution
    // Calculate width and height from SPS parameters
    *coded_width = sps.pic_width_max_in_luma_samples;
    *coded_height = sps.pic_height_max_in_luma_samples;
    
    // Handle conformance window if present
    if (sps.conformance_window_present_flag) {
      *coded_width -= (sps.conf_win_left_offset + sps.conf_win_right_offset) *
                      sps.sub_width_c;
      *coded_height -= (sps.conf_win_top_offset + sps.conf_win_bottom_offset) *
                       sps.sub_height_c;
    }
    
    // Set pixel aspect ratio if available
    if (sps.vui_parameters_present && 
        sps.vui_parameters.vui_aspect_ratio_info_present_flag) {
      switch (sps.vui_parameters.vui_aspect_ratio_idc) {
        case 1: *pixel_width = 1; *pixel_height = 1; break;  // Square
        case 2: *pixel_width = 12; *pixel_height = 11; break;
        case 3: *pixel_width = 10; *pixel_height = 11; break;
        case 4: *pixel_width = 16; *pixel_height = 11; break;
        case 5: *pixel_width = 40; *pixel_height = 33; break;
        case 6: *pixel_width = 24; *pixel_height = 11; break;
        case 7: *pixel_width = 20; *pixel_height = 11; break;
        case 8: *pixel_width = 32; *pixel_height = 11; break;
        case 9: *pixel_width = 80; *pixel_height = 33; break;
        case 10: *pixel_width = 18; *pixel_height = 11; break;
        case 11: *pixel_width = 15; *pixel_height = 11; break;
        case 12: *pixel_width = 64; *pixel_height = 33; break;
        case 13: *pixel_width = 160; *pixel_height = 99; break;
        case 14: *pixel_width = 4; *pixel_height = 3; break;
        case 15: *pixel_width = 3; *pixel_height = 2; break;
        case 16: *pixel_width = 2; *pixel_height = 1; break;
        case 255:  // Extended SAR
          *pixel_width = sps.vui_parameters.vui_sar_width;
          *pixel_height = sps.vui_parameters.vui_sar_height;
          break;
        default:
          *pixel_width = 1;
          *pixel_height = 1;
          break;
      }
    }
    
    return *coded_width > 0 && *coded_height > 0;
  }
 */ 
