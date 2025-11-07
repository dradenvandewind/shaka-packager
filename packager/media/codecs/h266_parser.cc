// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/codecs/h266_parser.h>

#include <algorithm>
#include <cmath>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/macros/compiler.h>
#include <packager/macros/logging.h>
#include <packager/media/codecs/nalu_reader.h>

#define TRUE_OR_RETURN(a)                            \
  do {                                               \
    if (!(a)) {                                      \
      DVLOG(1) << "Failure while processing " << #a; \
      return kInvalidStream;                         \
    }                                                \
  } while (0)

#define OK_OR_RETURN(a)  \
  do {                   \
    Result status = (a); \
    if (status != kOk)   \
      return status;     \
  } while (false)

#define READ_LONG_OR_RETURN(out)                                           \
  do {                                                                     \
    int _top_half, _bottom_half;                                           \
    if (!br->ReadBits(16, &_top_half)) {                                   \
      DVLOG(1)                                                             \
          << "Error in stream: unexpected EOS while trying to read " #out; \
      return kInvalidStream;                                               \
    }                                                                      \
    if (!br->ReadBits(16, &_bottom_half)) {                                \
      DVLOG(1)                                                             \
          << "Error in stream: unexpected EOS while trying to read " #out; \
      return kInvalidStream;                                               \
    }                                                                      \
    *(out) = ((long)_top_half) << 16 | _bottom_half;                       \
  } while (false)

namespace shaka {
namespace media {

namespace {
void GetAspectRatioInfo(const H266Sps& sps,
                        uint32_t* pixel_width,
                        uint32_t* pixel_height) {
  // The default value is 0; so if this is not in the SPS, it will correctly
  // assume unspecified.
  int aspect_ratio_idc = sps.vui_parameters.aspect_ratio_idc;

  // Table E.1 (extended for H.266)
  switch (aspect_ratio_idc) {
    case 1:  *pixel_width = 1;   *pixel_height = 1;  break;
    case 2:  *pixel_width = 12;  *pixel_height = 11; break;
    case 3:  *pixel_width = 10;  *pixel_height = 11; break;
    case 4:  *pixel_width = 16;  *pixel_height = 11; break;
    case 5:  *pixel_width = 40;  *pixel_height = 33; break;
    case 6:  *pixel_width = 24;  *pixel_height = 11; break;
    case 7:  *pixel_width = 20;  *pixel_height = 11; break;
    case 8:  *pixel_width = 32;  *pixel_height = 11; break;
    case 9:  *pixel_width = 80;  *pixel_height = 33; break;
    case 10: *pixel_width = 18;  *pixel_height = 11; break;
    case 11: *pixel_width = 15;  *pixel_height = 11; break;
    case 12: *pixel_width = 64;  *pixel_height = 33; break;
    case 13: *pixel_width = 160; *pixel_height = 99; break;
    case 14: *pixel_width = 4;   *pixel_height = 3;  break;
    case 15: *pixel_width = 3;   *pixel_height = 2;  break;
    case 16: *pixel_width = 2;   *pixel_height = 1;  break;
    case 17: *pixel_width = 3;   *pixel_height = 1;  break;  // H.266 extension

    case H266VuiParameters::kExtendedSar:
      *pixel_width = sps.vui_parameters.sar_width;
      *pixel_height = sps.vui_parameters.sar_height;
      break;

    default:
      // Section E.3.1 specifies that other values should be interpreted as 0.
      LOG(WARNING) << "Unknown aspect_ratio_idc " << aspect_ratio_idc;
      FALLTHROUGH_INTENDED;
    case 0:
      // Unlike the spec, assume 1:1 if not specified.
      *pixel_width = 1;
      *pixel_height = 1;
      break;
  }
}
}  // namespace

bool ExtractResolutionFromSps(const H266Sps& sps,
                              uint32_t* coded_width,
                              uint32_t* coded_height,
                              uint32_t* pixel_width,
                              uint32_t* pixel_height) {
  int crop_x = 0;
  int crop_y = 0;
  if (sps.conformance_window_present_flag) {
    int sub_width_c = 1;
    int sub_height_c = 1;

    // Table 6-1 for H.266
    switch (sps.chroma_format_idc) {
      case 0:  // Monochrome
        sub_width_c = 1;
        sub_height_c = 1;
        break;
      case 1:  // 4:2:0
        sub_width_c = 2;
        sub_height_c = 2;
        break;
      case 2:  // 4:2:2
        sub_width_c = 2;
        sub_height_c = 1;
        break;
      case 3:  // 4:4:4
        sub_width_c = 1;
        sub_height_c = 1;
        break;
      default:
        LOG(ERROR) << "Unexpected chroma_format_idc " << sps.chroma_format_idc;
        return false;
    }
   /* 
    croppedWidth = pic_width_in_luma_samples − SubWidthC * ( conf_win_right_offset + conf_win_left_offset ) (D-28) 
     croppedHeight = pic_height_in_luma_samples −SubHeightC * ( conf_win_bottom_offset + conf_win_top_offset ) (D-29)
   */
    // Formula similar to H.265 but with H.266 field names
    crop_x =
        sub_width_c * (sps.conf_win_right_offset + sps.conf_win_left_offset);
    crop_y =
        sub_height_c * (sps.conf_win_bottom_offset + sps.conf_win_top_offset);
  }

  // Calculate coded resolution after cropping
  *coded_width = sps.pic_width_max_in_luma_samples - crop_x;
  *coded_height = sps.pic_height_max_in_luma_samples - crop_y;
  GetAspectRatioInfo(sps, pixel_width, pixel_height);
  return true;
}

H266Pps::H266Pps() {}
H266Pps::~H266Pps() {}

H266Sps::H266Sps() {}
H266Sps::~H266Sps() {}

H266Vps::H266Vps() {}
H266Vps::~H266Vps() {}

H266Aps::H266Aps() {}
H266Aps::~H266Aps() {}

H266PictureHeader::H266PictureHeader() {}
H266PictureHeader::~H266PictureHeader() {}

H266SliceHeader::H266SliceHeader() {}
H266SliceHeader::~H266SliceHeader() {}

int H266Sps::GetPicSizeInCtbsY() const {
  // H.266 uses different calculation than H.265
  int min_cb_log2_size_y = log2_min_luma_coding_block_size_minus2 + 2;
  int ctb_log2_size_y = min_cb_log2_size_y + log2_ctu_size_minus5 + 5;
  int ctb_size_y = 1 << ctb_log2_size_y;

  // Round-up division.
  int pic_width_in_ctbs_y = (pic_width_max_in_luma_samples - 1) / ctb_size_y + 1;
  int pic_height_in_ctbs_y = (pic_height_max_in_luma_samples - 1) / ctb_size_y + 1;
  return pic_width_in_ctbs_y * pic_height_in_ctbs_y;
}

int H266Sps::GetChromaArrayType() const {
  return chroma_format_idc;  // H.266 doesn't have separate_colour_plane_flag
}

uint32_t H266Sps::GetBitDepthLuma() const {
    return 8 + bit_depth_luma_minus8;
  }
  
  uint32_t H266Sps::GetBitDepthChroma() const {
    return 8 + bit_depth_chroma_minus8;
  }
  
  uint32_t H266Sps::GetQpBdOffset() const {
    return qp_bd_offset;
  }
  
  // Vérification des plages valides
  bool H266Sps::IsValidBitDepth() const {
    return (bit_depth_luma_minus8 <= 8) && (bit_depth_chroma_minus8 <= 8);
  }


H266Parser::H266Parser() {}
H266Parser::~H266Parser() {}

H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu,
                                                H266SliceHeader* slice_header) {
  DCHECK(nalu.is_video_slice());
  *slice_header = H266SliceHeader();

  // Parses whole element.
  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  TRUE_OR_RETURN(br->ReadBool(&slice_header->first_slice_segment_in_pic_flag));
  
  // H.266 slice header starts differently than H.265
  if (nalu.type() >= Nalu::H266_IDR_W_RADL &&
      nalu.type() <= Nalu::H266_GDR_NUT) {
    TRUE_OR_RETURN(br->ReadBool(&slice_header->no_output_of_prior_pics_flag));
  }

  TRUE_OR_RETURN(br->ReadUE(&slice_header->pic_parameter_set_id));
  const H266Pps* pps = GetPps(slice_header->pic_parameter_set_id);
  TRUE_OR_RETURN(pps);

  const H266Sps* sps = GetSps(pps->seq_parameter_set_id);
  TRUE_OR_RETURN(sps);

  // H.266 has simpler slice header structure in some cases
  if (!slice_header->first_slice_segment_in_pic_flag) {
    if (pps->slice_header_extension_present_flag) {
      TRUE_OR_RETURN(br->ReadBool(&slice_header->dependent_slice_segment_flag));
    }
    
    // In H.266, segment address calculation is different
    const int bit_length = ceil(log2(sps->GetPicSizeInCtbsY()));
    if (bit_length > 0) {
      TRUE_OR_RETURN(br->ReadBits(bit_length, &slice_header->slice_segment_address));
    }
  }

  if (!slice_header->dependent_slice_segment_flag) {
    // H.266 slice type parsing
    TRUE_OR_RETURN(br->ReadUE(&slice_header->slice_type));
    
    // Simplified parsing for H.266 - many fields are handled differently
    if (nalu.type() != Nalu::H266_IDR_W_RADL &&
        nalu.type() != Nalu::H266_IDR_N_LP) {
      // Picture order count handling in H.266
      // This is simplified - actual H.266 POC is more complex
    }

    // Reference picture lists in H.266
    if (slice_header->slice_type == kPSlice ||
        slice_header->slice_type == kBSlice) {
      TRUE_OR_RETURN(br->ReadBool(&slice_header->slice_rpl_present_flag));
      
      if (slice_header->slice_rpl_present_flag) {
        TRUE_OR_RETURN(br->ReadUE(&slice_header->num_ref_idx_l0_active_minus1));
        if (slice_header->slice_type == kBSlice) {
          TRUE_OR_RETURN(br->ReadUE(&slice_header->num_ref_idx_l1_active_minus1));
        }
      }
    }

    // Quantization parameters
    TRUE_OR_RETURN(br->ReadSE(&slice_header->slice_qp_delta));
    
    if (pps->chroma_tool_offsets_present_flag) {
      TRUE_OR_RETURN(br->ReadSE(&slice_header->slice_cb_qp_offset));
      TRUE_OR_RETURN(br->ReadSE(&slice_header->slice_cr_qp_offset));
    }

    // Deblocking filter
    if (pps->deblocking_filter_override_enabled_flag) {
      TRUE_OR_RETURN(
          br->ReadBool(&slice_header->slice_deblocking_filter_override_flag));
    }
    
    if (slice_header->slice_deblocking_filter_override_flag) {
      TRUE_OR_RETURN(
          br->ReadBool(&slice_header->slice_deblocking_filter_disabled_flag));
      if (!slice_header->slice_deblocking_filter_disabled_flag) {
        TRUE_OR_RETURN(br->ReadSE(&slice_header->slice_beta_offset_div2));
        TRUE_OR_RETURN(br->ReadSE(&slice_header->slice_tc_offset_div2));
      }
    }
  }

  OK_OR_RETURN(ByteAlignment(br));

  slice_header->header_bit_size = nalu.payload_size() * 8 - br->NumBitsLeft();
  return kOk;
}

H266Parser::Result H266Parser::ParsePps(const Nalu& nalu, int* pps_id) {
  DCHECK_EQ(Nalu::H266_PPS_NUT, nalu.type());

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *pps_id = -1;
  std::unique_ptr<H266Pps> pps(new H266Pps);

  TRUE_OR_RETURN(br->ReadUE(&pps->pic_parameter_set_id));
  TRUE_OR_RETURN(br->ReadUE(&pps->seq_parameter_set_id));

  // H.266 PPS has different fields than H.265
  TRUE_OR_RETURN(br->ReadBool(&pps->no_qp_delta_flag));
  TRUE_OR_RETURN(br->ReadSE(&pps->init_qp_minus26));
  
  TRUE_OR_RETURN(br->ReadBool(&pps->cu_qp_delta_enabled_flag));
  if (pps->cu_qp_delta_enabled_flag) {
    TRUE_OR_RETURN(br->ReadUE(&pps->cu_chroma_qp_offset_list_len_minus1));
  }

  // Deblocking filter parameters
  TRUE_OR_RETURN(br->ReadBool(&pps->deblocking_filter_override_enabled_flag));
  TRUE_OR_RETURN(br->ReadBool(&pps->deblocking_filter_disabled_flag));
  TRUE_OR_RETURN(br->ReadSE(&pps->deblocking_filter_beta_offset_div2));
  TRUE_OR_RETURN(br->ReadSE(&pps->deblocking_filter_tc_offset_div2));

  // Weighted prediction
  TRUE_OR_RETURN(br->ReadBool(&pps->weighted_pred_flag));
  TRUE_OR_RETURN(br->ReadBool(&pps->weighted_bipred_flag));

  // Tiles
  TRUE_OR_RETURN(br->ReadBool(&pps->tiles_enabled_flag));
  if (pps->tiles_enabled_flag) {
    TRUE_OR_RETURN(br->ReadBool(&pps->uniform_tile_spacing_flag));
    TRUE_OR_RETURN(br->ReadUE(&pps->num_tile_columns_minus1));
    TRUE_OR_RETURN(br->ReadUE(&pps->num_tile_rows_minus1));
    
    if (!pps->uniform_tile_spacing_flag) {
      pps->tile_column_width_minus1.resize(pps->num_tile_columns_minus1);
      for (int i = 0; i < pps->num_tile_columns_minus1; i++) {
        TRUE_OR_RETURN(br->ReadUE(&pps->tile_column_width_minus1[i]));
      }
      
      pps->tile_row_height_minus1.resize(pps->num_tile_rows_minus1);
      for (int i = 0; i < pps->num_tile_rows_minus1; i++) {
        TRUE_OR_RETURN(br->ReadUE(&pps->tile_row_height_minus1[i]));
      }
    }
    
    TRUE_OR_RETURN(br->ReadBool(&pps->loop_filter_across_tiles_enabled_flag));
  }

  // Additional H.266 PPS fields would be parsed here...

  // This will replace any existing PPS instance.
  *pps_id = pps->pic_parameter_set_id;
  active_ppses_[*pps_id] = std::move(pps);

  return kOk;
}

H266Parser::Result H266Parser::ParseSps(const Nalu& nalu, int* sps_id) {
  DCHECK_EQ(Nalu::H266_SPS_NUT, nalu.type());

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *sps_id = -1;
  std::unique_ptr<H266Sps> sps(new H266Sps);

  TRUE_OR_RETURN(br->ReadUE(&sps->sps_seq_parameter_set_id));
  TRUE_OR_RETURN(br->ReadUE(&sps->vps_id));
  TRUE_OR_RETURN(br->ReadBits(3, &sps->max_sublayers_minus1));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_temporal_id_nesting_flag));

  // Profile/Tier/Level - simplified parsing
  for (int i = 0; i < 12; i++) {
    TRUE_OR_RETURN(br->ReadBits(8, &sps->general_profile_tier_level_data[i]));
  }

  // Chroma format and resolution
  TRUE_OR_RETURN(br->ReadUE(&sps->chroma_format_idc));
  TRUE_OR_RETURN(br->ReadUE(&sps->pic_width_max_in_luma_samples));
  TRUE_OR_RETURN(br->ReadUE(&sps->pic_height_max_in_luma_samples));

  // Conformance window
  TRUE_OR_RETURN(br->ReadBool(&sps->conformance_window_present_flag));
  if (sps->conformance_window_present_flag) {
    TRUE_OR_RETURN(br->ReadUE(&sps->conf_win_left_offset));
    TRUE_OR_RETURN(br->ReadUE(&sps->conf_win_right_offset));
    TRUE_OR_RETURN(br->ReadUE(&sps->conf_win_top_offset));
    TRUE_OR_RETURN(br->ReadUE(&sps->conf_win_bottom_offset));
  }

  // Bit depth
  TRUE_OR_RETURN(br->ReadUE(&sps->bit_depth_luma_minus8));
  TRUE_OR_RETURN(br->ReadUE(&sps->bit_depth_chroma_minus8));

  // Partitioning parameters
  TRUE_OR_RETURN(br->ReadUE(&sps->log2_ctu_size_minus5));
  TRUE_OR_RETURN(br->ReadUE(&sps->log2_min_luma_coding_block_size_minus2));

  // Quantization
  TRUE_OR_RETURN(br->ReadUE(&sps->qp_bd_offset));

  // Coding tools
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_temporal_mvp_enabled_flag));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_strong_intra_smoothing_enabled_flag));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_sao_enabled_flag));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_alf_enabled_flag));

  // VUI parameters
  TRUE_OR_RETURN(br->ReadBool(&sps->vui_parameters_present));
  if (sps->vui_parameters_present) {
    OK_OR_RETURN(ParseVuiParameters(sps->max_sublayers_minus1, br,
                                    &sps->vui_parameters));
  }

  // This will replace any existing SPS instance.
  *sps_id = sps->sps_seq_parameter_set_id;
  active_spses_[*sps_id] = std::move(sps);

  return kOk;
}
#if 0
H266Parser::Result H266Parser::ParseVps(const Nalu& nalu, int* vps_id) {
  DCHECK_EQ(Nalu::H266_VPS_NUT, nalu.type());

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *vps_id = -1;
  std::unique_ptr<H266Vps> vps(new H266Vps);

  TRUE_OR_RETURN(br->ReadUE(&vps->vps_video_parameter_set_id));
  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layers_minus1));
  TRUE_OR_RETURN(br->ReadBits(3, &vps->vps_max_sublayers_minus1));

  // Timing info in VPS (H.266 specific)
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_timing_info_present_flag));
  if (vps->vps_timing_info_present_flag) {
    READ_LONG_OR_RETURN(&vps->vps_num_units_in_tick);
    READ_LONG_OR_RETURN(&vps->vps_time_scale);
  }

  // General constraints
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_each_layer_is_an_ols_flag));
  TRUE_OR_RETURN(br->ReadBits(2, &vps->vps_ols_mode_idc));

  // Output layer sets
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_num_output_layer_sets_minus1));
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_num_ptls_minus1));

  // Profile tier level data
  for (int i = 0; i <= vps->vps_num_ptls_minus1; i++) {
    for (int j = 0; j < kGeneralProfileTierLevelBytes; j++) {
      TRUE_OR_RETURN(br->ReadBits(8, &vps->general_profile_tier_level_data[i][j]));
    }
  }

  // Layer sets
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_num_layer_sets_minus1));
  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layer_id));

  // OPI support
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_opi_present_flag));

  // This will replace any existing VPS instance.
  *vps_id = vps->vps_video_parameter_set_id;
  active_vpses_[*vps_id] = std::move(vps);

  return kOk;
}
#endif 

H266Parser::Result H266Parser::ParseAps(const Nalu& nalu, int* aps_id, int* aps_type) {
  DCHECK(nalu.type() == Nalu::H266_PREFIX_APS_NUT || 
         nalu.type() == Nalu::H266_SUFFIX_APS_NUT);

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *aps_id = -1;
  *aps_type = -1;
  std::unique_ptr<H266Aps> aps(new H266Aps);

  TRUE_OR_RETURN(br->ReadUE(&aps->aps_type));
  TRUE_OR_RETURN(br->ReadUE(&aps->aps_id));

  // Simplified APS parsing - actual implementation would parse type-specific data
  // ALF, LMCS, or scaling list parameters would be parsed here based on aps_type

  *aps_id = aps->aps_id;
  *aps_type = aps->aps_type;
  active_apses_[*aps_id] = std::move(aps);

  return kOk;
}

H266Parser::Result H266Parser::ParsePictureHeader(const Nalu& nalu,
                                                  H266PictureHeader* picture_header) {
  DCHECK_EQ(Nalu::H266_PH_NUT, nalu.type());

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *picture_header = H266PictureHeader();

  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_gdr_or_irap_pic_flag));
  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_non_ref_pic_flag));
  TRUE_OR_RETURN(br->ReadUE(&picture_header->ph_pic_parameter_set_id));

  // Reference picture lists in PH
  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_rpl_present_flag));

  // Deblocking filter
  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_deblocking_filter_override_flag));
  if (picture_header->ph_deblocking_filter_override_flag) {
    TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_deblocking_filter_disabled_flag));
    if (!picture_header->ph_deblocking_filter_disabled_flag) {
      TRUE_OR_RETURN(br->ReadSE(&picture_header->ph_beta_offset_div2));
      TRUE_OR_RETURN(br->ReadSE(&picture_header->ph_tc_offset_div2));
    }
  }

  // Quantization
  TRUE_OR_RETURN(br->ReadSE(&picture_header->ph_qp_delta));

  // Weighted prediction
  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_weighted_pred_flag));
  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_weighted_bipred_flag));

  // Temporal MVP
  TRUE_OR_RETURN(br->ReadBool(&picture_header->ph_temporal_mvp_enabled_flag));

  return kOk;
}

const H266Pps* H266Parser::GetPps(int pps_id) {
  return active_ppses_[pps_id].get();
}

const H266Sps* H266Parser::GetSps(int sps_id) {
  return active_spses_[sps_id].get();
}

const H266Vps* H266Parser::GetVps(int vps_id) {
  //return active_vpses_[vps_id].get();
  auto it = active_vpses_.find(vps_id);
  return it != active_vpses_.end() ? it->second.get() : nullptr;
}

bool H266Parser::GetVpsTimingInfo(int vps_id, uint32_t* num_units_in_tick, 
                                 uint32_t* time_scale) {
  const H266Vps* vps = GetVps(vps_id);
  if (!vps || !vps->vps_timing_info_present_flag) {
    return false;
  }
  
  *num_units_in_tick = vps->vps_num_units_in_tick;
  *time_scale = vps->vps_time_scale;
  return true;
}

uint32_t H266Parser::GetMaxLayers(int vps_id) {
  const H266Vps* vps = GetVps(vps_id);
  return vps ? (vps->vps_max_layers_minus1 + 1) : 1;
}

bool H266Parser::IsLayerIndependent(int vps_id, uint32_t layer_id) {
  const H266Vps* vps = GetVps(vps_id);
  if (!vps || layer_id > static_cast<uint32_t>(vps->vps_max_layers_minus1)) {
    return false;
  }
  
  if (vps->vps_all_independent_layers_flag) {
    return true;
  }
  
  // Check if this layer has no dependencies
  for (uint32_t i = 0; i < layer_id; i++) {
    if (vps->direct_dependency_flag[layer_id][i]) {
      return false;
    }
  }
  return true;
}

const H266Aps* H266Parser::GetAps(int aps_id) {
  return active_apses_[aps_id].get();
}

H266Parser::Result H266Parser::ParseVuiParameters(int max_num_sub_layers_minus1,
                                                  H26xBitReader* br,
                                                  H266VuiParameters* vui) {
  // Reads whole element but ignores most of it.
  int ignored;

  TRUE_OR_RETURN(br->ReadBool(&vui->aspect_ratio_info_present_flag));
  if (vui->aspect_ratio_info_present_flag) {
    TRUE_OR_RETURN(br->ReadBits(8, &vui->aspect_ratio_idc));
    if (vui->aspect_ratio_idc == H266VuiParameters::kExtendedSar) {
      TRUE_OR_RETURN(br->ReadBits(16, &vui->sar_width));
      TRUE_OR_RETURN(br->ReadBits(16, &vui->sar_height));
    }
  }

  // Skip various VUI flags and parameters
  bool overscan_info_present_flag;
  TRUE_OR_RETURN(br->ReadBool(&overscan_info_present_flag));
  if (overscan_info_present_flag) {
    TRUE_OR_RETURN(br->SkipBits(1));  // overscan_appropriate_flag
  }

  bool video_signal_type_present_flag;
  TRUE_OR_RETURN(br->ReadBool(&video_signal_type_present_flag));
  if (video_signal_type_present_flag) {
    TRUE_OR_RETURN(br->SkipBits(3));  // video_format
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_full_range_flag));

    TRUE_OR_RETURN(br->ReadBool(&vui->vui_color_description_present_flag));
    if (vui->vui_color_description_present_flag) {
      TRUE_OR_RETURN(br->ReadBits(8, &vui->color_primaries));
      TRUE_OR_RETURN(br->ReadBits(8, &vui->transfer_characteristics));
      TRUE_OR_RETURN(br->ReadBits(8, &vui->matrix_coefficients));
    }
  }

  TRUE_OR_RETURN(br->ReadBool(&vui->vui_chroma_loc_info_present_flag));
  if (vui->vui_chroma_loc_info_present_flag) {
    TRUE_OR_RETURN(br->ReadUE(&vui->vui_chroma_sample_loc_type_frame));
    TRUE_OR_RETURN(br->ReadUE(&vui->vui_chroma_sample_loc_type_top_field));
    TRUE_OR_RETURN(br->ReadUE(&vui->vui_chroma_sample_loc_type_bottom_field));
  }

  // Timing info
  TRUE_OR_RETURN(br->ReadBool(&vui->vui_timing_info_present_flag));
  if (vui->vui_timing_info_present_flag) {
    READ_LONG_OR_RETURN(&vui->vui_num_units_in_tick);
    READ_LONG_OR_RETURN(&vui->vui_time_scale);
  }

  // Bitstream restriction
  TRUE_OR_RETURN(br->ReadBool(&vui->bitstream_restriction_flag));
  if (vui->bitstream_restriction_flag) {
    TRUE_OR_RETURN(br->ReadUE(&vui->min_spatial_segmentation_idc));
    // Skip other restriction parameters
    TRUE_OR_RETURN(br->ReadUE(&ignored));  // max_bytes_per_pic_denom
    TRUE_OR_RETURN(br->ReadUE(&ignored));  // max_bits_per_min_cu_denum
  }

  return kOk;
}

H266Parser::Result H266Parser::ParseProfileTierLevel(bool profile_tier_present,
                                                     int max_num_sub_layers_minus1,
                                                     H26xBitReader* br) {
  // Simplified profile/tier/level parsing for H.266
  if (profile_tier_present) {
    // Skip general_profile_tier_level data
    TRUE_OR_RETURN(br->SkipBits(12 * 8));  // 12 bytes
  }

  // Skip sublayer profile/tier/level info
  for (int i = 0; i < max_num_sub_layers_minus1; i++) {
    bool sublayer_profile_present, sublayer_level_present;
    TRUE_OR_RETURN(br->ReadBool(&sublayer_profile_present));
    TRUE_OR_RETURN(br->ReadBool(&sublayer_level_present));
    
    if (sublayer_profile_present) {
      TRUE_OR_RETURN(br->SkipBits(2 + 1 + 5 + 32 + 4 + 43 + 1));
    }
    if (sublayer_level_present) {
      TRUE_OR_RETURN(br->SkipBits(8));
    }
  }

  return kOk;
}

H266Parser::Result H266Parser::SkipScalingListData(H26xBitReader* br) {
  // H.266 scaling list data parsing would go here
  // Similar to H.265 but with potential differences
  int ignored;
  for (int size_id = 0; size_id < 4; size_id++) {
    for (int matrix_id = 0; matrix_id < 6;
         matrix_id += ((size_id == 3) ? 3 : 1)) {
      bool scaling_list_pred_mode;
      TRUE_OR_RETURN(br->ReadBool(&scaling_list_pred_mode));
      if (!scaling_list_pred_mode) {
        TRUE_OR_RETURN(br->ReadUE(&ignored));  // scaling_list_pred_matrix_id_delta
      } else {
        int coefNum = std::min(64, (1 << (4 + (size_id << 1))));
        if (size_id > 1) {
          TRUE_OR_RETURN(br->ReadSE(&ignored));  // scaling_list_dc_coef_minus8
        }
        for (int i = 0; i < coefNum; i++) {
          TRUE_OR_RETURN(br->ReadSE(&ignored));  // scaling_list_delta_coef
        }
      }
    }
  }
  return kOk;
}

H266Parser::Result H266Parser::ByteAlignment(H26xBitReader* br) {
  TRUE_OR_RETURN(br->SkipBits(1));
  TRUE_OR_RETURN(br->SkipBits(br->NumBitsLeft() % 8));
  return kOk;
}

// Stub implementations for methods that need to be defined
H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu, 
                                               H266SliceHeader* slice_header,
                                               const H266PictureHeader* picture_header) {
  // Implementation would use picture_header context

  //todo 
  return ParseSliceHeader(nalu, slice_header);
}
#if 0
//future update perhaps
H266Parser::Result H266Parser::ParseDci(const Nalu& nalu, H266DecodingCapabilityInfo* dci) {
  // Stub implementation
  return kOk;
}


H266Parser::Result H266Parser::ParseOpi(const Nalu& nalu, H266OperatingPointInfo* opi) {
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::ParseSei(const Nalu& nalu, H266SEIMessage* sei_msg) {
  // Stub implementation
  return kOk;
}
#endif
#if 0   
//future update perhaps
H266Parser::Result H266Parser::ParseReferencePictureList(const H266Sps& sps,
                                                        const H266Pps& pps,
                                                        H26xBitReader* br,
                                                        H266SliceHeader* slice_header) {
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::SkipAlfData(H26xBitReader* br) {
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::SkipLmcsData(H26xBitReader* br) {
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::ParseOlsIds(H26xBitReader* br, std::vector<int>* ols_ids) {
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::ParseDpbParameters(int max_sublayers_minus1,
                                                 bool sublayer_info_flag,
                                                 H26xBitReader* br) {
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::ParseGeneralConstraintsInfo(H26xBitReader* br) {
  // Stub implementation
  return kOk;
}
#endif 
H266Parser::Result H266Parser::ParseVps(const Nalu& nalu, int* vps_id) {
  DCHECK_EQ(Nalu::H266_VPS_NUT, nalu.type());

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *vps_id = -1;
  std::unique_ptr<H266Vps> vps(new H266Vps);

  // VPS header
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_video_parameter_set_id));
  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layers_minus1));
  TRUE_OR_RETURN(br->ReadBits(3, &vps->vps_max_sublayers_minus1));
  
  // VPS base layer info
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_all_independent_layers_flag));
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_default_output_layer_idc));

  // Layer IDs
  vps->layer_id_included_flag.resize(vps->vps_max_layers_minus1 + 1, false);
for (uint32_t i = 1; i <= (vps->vps_max_layers_minus1); i++) {
       bool temp_flag;
       TRUE_OR_RETURN(br->ReadBool(&temp_flag));
       vps->layer_id_included_flag[i] = temp_flag;
      //TRUE_OR_RETURN(br->ReadBool(&vps->layer_id_included_flag[i]));
  }

  // Timing info
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_timing_info_present_flag));
  if (vps->vps_timing_info_present_flag) {
    READ_LONG_OR_RETURN(&vps->vps_num_units_in_tick);
    READ_LONG_OR_RETURN(&vps->vps_time_scale);
    
    TRUE_OR_RETURN(br->ReadBool(&vps->vps_poc_proportional_to_timing_flag));
    if (vps->vps_poc_proportional_to_timing_flag) {
      
    int temp_int;
    TRUE_OR_RETURN(br->ReadUE(&temp_int));
    vps->vps_num_ticks_poc_diff_one_minus1 = static_cast<uint32_t>(temp_int);    }
  }

  // Output layer sets
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_num_output_layer_sets));
  
  // Allocate and parse output layer flags
  vps->output_layer_flag.resize(vps->vps_num_output_layer_sets);
  for (uint32_t i = 1; i <= vps->vps_num_output_layer_sets; i++) {
    vps->output_layer_flag[i].resize(vps->vps_max_layers_minus1 + 1, false);
    for (uint32_t j = 0; j <= vps->vps_max_layers_minus1; j++) {

      //TRUE_OR_RETURN(br->ReadBool(&vps->output_layer_flag[i][j]));
      bool temp_output_bool;
      TRUE_OR_RETURN(br->ReadBool(&temp_output_bool));
      vps->output_layer_flag[i][j] = temp_output_bool;
    }
  }

  // Profile Tier Level parsing
  OK_OR_RETURN(ParseProfileTierLevel(true, vps->vps_max_sublayers_minus1, br, 
                                    &vps->profile_tier_level));

  // Layer dependency information
  if (!vps->vps_all_independent_layers_flag) {
    vps->direct_dependency_flag.resize(vps->vps_max_layers_minus1 + 1);
    vps->max_tid_ref_present_flag.resize(vps->vps_max_layers_minus1 + 1, false);
    
    for (uint32_t i = 1; i <= vps->vps_max_layers_minus1; i++) {
      vps->direct_dependency_flag[i].resize(vps->vps_max_layers_minus1 + 1, false);
      for (uint32_t j = 0; j < i; j++) {
        //TRUE_OR_RETURN(br->ReadBool(&vps->direct_dependency_flag[i][j]));
        bool temp_dep_bool;
        TRUE_OR_RETURN(br->ReadBool(&temp_dep_bool));
        vps->direct_dependency_flag[i][j] = temp_dep_bool;

      }
    }

    for (uint32_t i = 1; i <= vps->vps_max_layers_minus1; i++) {
      //TRUE_OR_RETURN(br->ReadBool(&vps->max_tid_ref_present_flag[i]));
      bool temp_tid_bool;
      TRUE_OR_RETURN(br->ReadBool(&temp_tid_bool));
      vps->max_tid_ref_present_flag[i] = temp_tid_bool;

    }
  }

  // Byte alignment
  OK_OR_RETURN(ByteAlignment(br));

  // Store the VPS
  *vps_id = vps->vps_video_parameter_set_id;
  active_vpses_[*vps_id] = std::move(vps);

  DVLOG(3) << "Successfully parsed VPS ID: " << *vps_id 
           << " with " << (vps->vps_max_layers_minus1 + 1) << " layers";

  return kOk;
}
H266Parser::Result H266Parser::ParseProfileTierLevel(bool profile_tier_present,
                                                     int max_num_sub_layers_minus1,
                                                     H26xBitReader* br,
                                                     H266ProfileTierLevel* ptl) {
  if (profile_tier_present) {
    // General profile tier level
    //TRUE_OR_RETURN(br->ReadBits(7, &ptl->general_profile_idc));
    //TRUE_OR_RETURN(br->ReadBool(&ptl->general_tier_flag));
    //TRUE_OR_RETURN(br->ReadBits(8, &ptl->general_level_idc));
    int temp_profile;
    TRUE_OR_RETURN(br->ReadBits(7, &temp_profile));
    ptl->general_profile_idc = static_cast<uint8_t>(temp_profile);

    bool temp_tier;
    TRUE_OR_RETURN(br->ReadBool(&temp_tier));
    ptl->general_tier_flag = temp_tier;

    int temp_level;
    TRUE_OR_RETURN(br->ReadBits(8, &temp_level));
    ptl->general_level_idc = static_cast<uint8_t>(temp_level);
    
    // Constraints flags
    uint32_t constraint_flags;
    TRUE_OR_RETURN(br->ReadBits(32, &constraint_flags));
    
    // Extra constraint flags for H.266
    uint32_t general_constraints_info;
    TRUE_OR_RETURN(br->ReadBits(43, &general_constraints_info));
    
    // Multi-layer info
    TRUE_OR_RETURN(br->ReadBool(&ptl->general_frame_only_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&ptl->general_non_packed_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&ptl->general_interlaced_source_flag));
    TRUE_OR_RETURN(br->ReadBool(&ptl->general_progressive_source_flag));
  }

  // Sub-layer profile tier level info
  for (int i = 0; i < max_num_sub_layers_minus1; i++) {
    bool sublayer_profile_present_flag, sublayer_level_present_flag;
    TRUE_OR_RETURN(br->ReadBool(&sublayer_profile_present_flag));
    TRUE_OR_RETURN(br->ReadBool(&sublayer_level_present_flag));
    
    if (sublayer_profile_present_flag) {
      // Skip sub-layer profile info
      TRUE_OR_RETURN(br->SkipBits(88)); // 7+1+8+32+43+1
    }
    if (sublayer_level_present_flag) {
      TRUE_OR_RETURN(br->SkipBits(8)); // sub_layer_level_idc[i]
    }
  }

  return kOk;
}
bool H266Parser::ParseNalUnits(const uint8_t* data,
                               size_t size,
                               std::vector<NalUnit>* nal_units) {
  if (!data || size == 0 || !nal_units) {
    LOG(ERROR) << "Invalid parameters to ParseNalUnits";
    return false;
  }

  nal_units->clear();

  // Use NaluReader to parse the bitstream
  NaluReader reader(Nalu::kH266, 0, data, size);
  
  Nalu nalu;
  NaluReader::Result result;
  
  while ((result = reader.Advance(&nalu)) == NaluReader::kOk) {
    // Create NalUnit entry
    NalUnit unit;
    unit.data = nalu.data();
    unit.size = nalu.header_size() + nalu.payload_size();
    unit.type = nalu.type();
    
    nal_units->push_back(unit);
    
    DVLOG(3) << "Found H.266 NAL unit: type=" << nalu.type() 
             << " size=" << unit.size;
  }
  
  if (result != NaluReader::kEOStream) {
    LOG(ERROR) << "Failed to parse H.266 NAL units, result: " << result;
    return false;
  }
  
  if (nal_units->empty()) {
    LOG(WARNING) << "No NAL units found in buffer";
    return false;
  }
  
  DVLOG(2) << "Successfully parsed " << nal_units->size() << " H.266 NAL units";
  return true;
}

}  // namespace media
}  // namespace shaka