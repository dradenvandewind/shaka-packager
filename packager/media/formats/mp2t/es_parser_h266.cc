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
//#include <packager/media/codecs/h266_parser.h>
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
  LOG(INFO) << "Processing H.266 NALU of type: " << nalu.type();

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

void EsParserH266::Reset() {
  LOG(INFO) << "Resetting EsParserH266 state.";
  current_access_unit_.clear();
  current_access_unit_pts_ = -1;
  current_access_unit_dts_ = -1;
  current_access_unit_is_keyframe_ = false;
  
  pending_samples_.clear();
  
  timestamp_tracker_.clear();
  last_frame_rate_ = 0.0;
  last_sample_duration_ = 0;
  
  pps_map_.clear();
  sps_map_.clear();
  vps_map_.clear();
  
  last_pps_.reset();
  last_sps_.reset();
  last_vps_.reset();
  
  waiting_for_keyframe_ = true;
  frames_parsed_ = 0;
  first_pts_ = -1;
  last_pts_ = -1;
  
  es_buffer_.clear();
}

  

namespace {
constexpr int64_t kMicrosecondsPerSecond = 1000000;
}




int64_t EsParserH266::GetSampleDurationFromSps(int pps_id) {
  LOG(INFO) << "Getting sample duration from SPS for PPS ID: " << pps_id;
  static constexpr int64_t kMinValidDuration = 1000;   // 1ms
  static constexpr int64_t kMaxValidDuration = 1000000; // 1s

  auto sps = GetSpsForPps(pps_id);
  if (!sps ) {
    return 0;
  }

  const auto& vui = sps->vui_parameters;
  
  if (!vui.vui_timing_info_present_flag) {
    return 0;
  }

  // Vérifications supplémentaires pour VVC
  // if (vui.field_seq_flag) {
  //   // Gestion spécifique pour le entrelacé (field sequential)
  //   return HandleFieldSequentialTiming(vui);
  // }

  

  //const auto& timing = vui.vui_timing_info_present_flag;
  
  // Validation renforcée
  if (vui.vui_time_scale == 0 || vui.vui_num_units_in_tick == 0) {
    LOG(ERROR) << "Invalid timing values: time_scale=" << vui.vui_time_scale
               << ", num_units_in_tick=" << vui.vui_num_units_in_tick;
    return 0;
  }

  // Calcul avec vérification de dépassement
  if (vui.vui_num_units_in_tick > (std::numeric_limits<int64_t>::max() / kMicrosecondsPerSecond)) {
    LOG(ERROR) << "Potential overflow in duration calculation";
    return 0;
  }

  int64_t duration = (kMicrosecondsPerSecond * vui.vui_num_units_in_tick) / vui.vui_time_scale;

  // Ajustement pour le HDR et les taux de rafraîchissement élevés
  // if (vui.hdr_parameters_present_flag) {
  //   duration = AdjustDurationForHdr(duration, vui);
  // }

  // Validation finale
  if (duration < kMinValidDuration || duration > kMaxValidDuration) {
    LOG(WARNING) << "Duration out of reasonable range: " << duration << "µs";
    return 0;
  }

  return duration;
}

int64_t EsParserH266::CalculateDurationFromRecentTimestamps() {
  LOG(INFO) << "Calculating duration from recent timestamps.";
  if (timestamp_tracker_.size() < 2) {
    return 0;
  }

  // Calculer la durée moyenne basée sur les timestamps récents
  int64_t total_duration = 0;
  int count = 0;

  for (size_t i = 1; i < timestamp_tracker_.size(); ++i) {
    int64_t duration = timestamp_tracker_[i].pts - timestamp_tracker_[i-1].pts;
    if (duration > 0 && duration < kMicrosecondsPerSecond) { // Filtrer les valeurs aberrantes
      total_duration += duration;
      count++;
    }
  }
  if (count > 0) {
    last_frame_rate_ = kMicrosecondsPerSecond / (total_duration / count);
    return total_duration / count;
  }

  return 0;
}

int64_t EsParserH266::GetDefaultSampleDuration() {
  LOG(INFO) << "Using default sample duration based on content type.";
  // Durées par défaut basées sur le type de contenu typique
   const int64_t kDefaultDurationUHD = 1000000 / 60;  // 60 fps pour UHD
  const int64_t kDefaultDurationHD = 1000000 / 30;   // 30 fps pour HD
  // const int64_t kDefaultDurationSD = 1000000 / 25;   // 25 fps pour SD

  // Essayer de déterminer la résolution depuis le SPS
  auto sps = GetLastActiveSps();
  if (sps) {
    int width = sps->pic_width_max_in_luma_samples;
    int height = sps->pic_height_max_in_luma_samples;
    
    if (width >= 3840 || height >= 2160) {
      return kDefaultDurationUHD;
    } else if (width >= 1920 || height >= 1080) {
      return kDefaultDurationHD;
    }
  }

  // Fallback ultra-conservateur
  return kDefaultDurationHD;
}
std::shared_ptr<H266Sps> EsParserH266::GetSpsForPps(int pps_id) {
  LOG(INFO) << "Getting SPS for PPS ID: " << pps_id;
  auto pps_iter = pps_map_.find(pps_id);
  if (pps_iter == pps_map_.end()) {
    return nullptr;
  }
  
  auto sps_iter = sps_map_.find(pps_iter->second->seq_parameter_set_id);
  return (sps_iter != sps_map_.end()) ? sps_iter->second : nullptr;
}

std::shared_ptr<H266Sps> EsParserH266::GetLastActiveSps() {
  LOG(INFO) << "Getting last active SPS.";
  if (!last_pps_) {
    return nullptr;
  }
  return GetSpsForPps(last_pps_->pic_parameter_set_id);
}



int64_t EsParserH266::CalculateSampleDuration(int pps_id) {
  LOG(INFO) << "Calculating sample duration for PPS ID: " << pps_id;
  // 1. Essayer d'obtenir la durée depuis les paramètres VVC (SPS)
  int64_t duration_from_sps = GetSampleDurationFromSps(pps_id);
  if (duration_from_sps > 0) {
    return duration_from_sps;
  }

  // 2. Utiliser le framerate détecté précédemment s'il existe
  if (last_frame_rate_ > 0) {
    return 1000000 / last_frame_rate_; // Convertir en microsecondes
  }

  // 3. Analyser les timestamps DTS/PTS récents pour calculer la durée
  int64_t duration_from_timestamps = CalculateDurationFromRecentTimestamps();
  if (duration_from_timestamps > 0) {
    return duration_from_timestamps;
  }

  // 4. Fallback: utiliser une durée par défaut basée sur le type de contenu
  return GetDefaultSampleDuration();
}


bool EsParserH266::ProcessVclNalu(const Nalu& nalu,
                                  VideoSliceInfo* video_slice_info) {
LOG(INFO) << "Processing VCL NALU of type: " << nalu.type();



  const bool is_key_frame = (nalu.type() == Nalu::H266_IDR_W_RADL ||
                               nalu.type() == Nalu::H266_IDR_N_LP);

  LOG(INFO) << "Nalu: slice KeyFrame=" << is_key_frame;


  // Parse slice header to get PPS ID and other information
  H266SliceHeader slice_header;
  auto status = (parser_->ParseSliceHeader(nalu, &slice_header));
  if ( status == H266Parser::kOk)
  {
    video_slice_info->valid = true;
    video_slice_info->is_key_frame = is_key_frame;
    video_slice_info->frame_num = 0; // frame_num is only for H264.
    video_slice_info->pps_id = slice_header.pic_parameter_set_id;
  } else if (status == H266Parser::kUnsupportedStream) {
    LOG(INFO) << "Unsupported feature in H.266 slice header.";
    new_stream_info_cb_(nullptr);  // Signal an error.
   
  } else {
          if(last_video_decoder_config_){
            return false;
          }
            
  }
  // nor sure to add additionnal code here 
  return true;

}

bool EsParserH266::ProcessOtherNonVclNalu(const Nalu& nalu) {
  LOG(INFO) << "Processing non-VCL NALU of type: " << nalu.type();
  switch (nalu.type()) {
    case Nalu::H266_VPS_NUT: {
      LOG(INFO) << "Processing VPS";
      int vps_id;
      auto status = parser_->ParseVps(nalu, &vps_id);
      if (status == H266Parser::kOk){
        // VPS parsed successfully
        decoder_config_check_pending_ = true;
      } else if (status == H266Parser::kUnsupportedStream){
          new_stream_info_cb_(nullptr);
      } else{
             
      return false;
      }
    
      break;
    }
    case Nalu::H266_SPS_NUT: {
      LOG(INFO) << "Processing SPS";
      int sps_id;
      auto status = parser_->ParseSps(nalu, &sps_id);
      if (status == H266Parser::kOk){
        // SPS parsed successfully
        decoder_config_check_pending_ = true;
      } else if (status == H266Parser::kUnsupportedStream){
          new_stream_info_cb_(nullptr);
      } else{
             
      return false;
      }

      break;
    }
    case Nalu::H266_PPS_NUT: {
      LOG(INFO) << "Processing PPS";
      int pps_id;
      auto status = parser_->ParsePps(nalu, &pps_id);
      if (status == H266Parser::kOk){
        // PPS parsed successfully
        decoder_config_check_pending_ = true;
      } else if (status == H266Parser::kUnsupportedStream){
          new_stream_info_cb_(nullptr);
      } else{
        // Allow PPS parsing to fail if waiting for SPS.
        if (last_video_decoder_config_)
          return false;
             
      return false;
      }
      break;
    }
    case Nalu::H266_AUD_NUT: {
      //decoding AUD information
      LOG(INFO) << "Processing AUD ";
      int aud_id;
      auto status = parser_->ParseAccessUnitDelimeter_Rbsp(nalu,&aud_id);
      if (status == H266Parser::kOk){
        //aud parsed successfully
        decoder_config_check_pending_= true;
      } else if (status == H266Parser::kUnsupportedStream){
            new_stream_info_cb_(nullptr);
      } else {
         if (last_video_decoder_config_){
          return false;
         }        
      }
      break;
    }
    case Nalu::H266_PH_NUT: {
      //decodgin picture header information
      LOG(INFO) << "Processing PH";

      H266PictureHeaderRbsp pictureheader;
      //ParsePictureHeaderRbsp

      auto status = parser_->ParsePictureHeaderRbsp(nalu, &pictureheader);
      if ( status == H266Parser::kOk)
      {
          LOG(INFO) << "Success Processing PH";
      } else if (status == H266Parser::kUnsupportedStream) {
          LOG(INFO) << "Unsupported feature in H.266 picture header rbsp.";
          new_stream_info_cb_(nullptr);  // Signal an error.
      } else {
          return false;                 
      }
      break;
    }    
    case Nalu::H266_DCI_NUT:
      // Decoding Capability Information
      break;
    case Nalu::H266_PREFIX_APS_NUT:
    case Nalu::H266_SUFFIX_APS_NUT:
      LOG(INFO) << "Processing APS need add processing";
      break;
    case Nalu::H266_OPI_NUT:
      // Operating Point Information
      break;
    case Nalu::H266_PREFIX_SEI_NUT:
    case Nalu::H266_SUFFIX_SEI_NUT:
      // SEI messages
      LOG(INFO) << "Processing SEI messages need implement section";
      break;
    default:
      // Other NALUs
      break;
  }
  return true;
}

bool EsParserH266::UpdateVideoDecoderConfig(int pps_id) {
  LOG(INFO) << "Updating video decoder config for PPS ID: " << pps_id;
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

      LOG(WARNING) << "H.266 decoder configuration has changed.";
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
  DLOG(INFO) << " ## ExtractResolutionFromSps coded_Size " << coded_width  << "x " << coded_height << " ## pixel_size" << pixel_width << "x " << pixel_height;

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