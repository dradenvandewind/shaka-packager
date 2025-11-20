// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/codecs/h266_parser.h>
#include "packager/media/codecs/h26x_bit_reader.h"


#include <algorithm>
#include <cmath>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/macros/compiler.h>
#include <packager/macros/logging.h>
#include <packager/media/codecs/nalu_reader.h>

// Forward declarations
struct GeneralTimingHrdParameters;
struct H266OlsTimingHrdParameters;
struct H266ProfileTierLevel;
struct H266GeneralConstraintsInfo;
struct H266DPB_Parameters;
struct H266PictureHeaderStructure;
struct H266ReferencePicList;
struct H266PredWeightTable;


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
/* ####################################################################################################################################*/
void DisplayH266SPS(const H266Sps& sps) {
    DLOG(INFO) << "=== H.266 SPS Parameters ===";
    
    // Basic parameters
    DLOG(INFO) << "## sps_seq_parameter_set_id : " << sps.sps_seq_parameter_set_id;
    DLOG(INFO) << "## vps_id : " << sps.vps_id;
    DLOG(INFO) << "## sps_video_parameter_set_id : " << sps.sps_video_parameter_set_id;
    DLOG(INFO) << "## max_sublayers_minus1 : " << sps.max_sublayers_minus1;
    DLOG(INFO) << "## sps_chroma_format_idc : " << sps.sps_chroma_format_idc;
    DLOG(INFO) << "## sps_log2_ctu_size_minus5 : " << sps.sps_log2_ctu_size_minus5;
    DLOG(INFO) << "## sps_ptl_dpb_hrd_params_present_flag : " << (sps.sps_ptl_dpb_hrd_params_present_flag ? "1" : "0");
    
    // GDR and resolution parameters
    DLOG(INFO) << "## sps_gdr_enabled_flag : " << (sps.sps_gdr_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_ref_pic_resampling_enabled_flag : " << (sps.sps_ref_pic_resampling_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_res_change_in_clvs_allowed_flag : " << (sps.sps_res_change_in_clvs_allowed_flag ? "1" : "0");
    DLOG(INFO) << "## sps_pic_width_in_luma_samples : " << sps.sps_pic_width_in_luma_samples;
    DLOG(INFO) << "## sps_pic_width_max_in_luma_samples : " << sps.sps_pic_width_max_in_luma_samples;
    DLOG(INFO) << "## sps_pic_height_max_in_luma_samples : " << sps.sps_pic_height_max_in_luma_samples;
    DLOG(INFO) << "## sps_pic_height_in_luma_samples : " << sps.sps_pic_height_in_luma_samples;
    
    // Conformance window
    DLOG(INFO) << "## sps_conformance_window_flag : " << (sps.sps_conformance_window_flag ? "1" : "0");
    if (sps.sps_conformance_window_flag) {
        DLOG(INFO) << "## sps_conf_win_left_offset : " << sps.sps_conf_win_left_offset;
        DLOG(INFO) << "## sps_conf_win_right_offset : " << sps.sps_conf_win_right_offset;
        DLOG(INFO) << "## sps_conf_win_top_offset : " << sps.sps_conf_win_top_offset;
        DLOG(INFO) << "## sps_conf_win_bottom_offset : " << sps.sps_conf_win_bottom_offset;
    }
    
    // Subpicture parameters
    DLOG(INFO) << "## sps_subpic_info_present_flag : " << (sps.sps_subpic_info_present_flag ? "1" : "0");
    if (sps.sps_subpic_info_present_flag) {
        DLOG(INFO) << "## sps_num_subpics_minus1 : " << sps.sps_num_subpics_minus1;
        DLOG(INFO) << "## sps_independent_subpics_flag : " << (sps.sps_independent_subpics_flag ? "1" : "0");
        DLOG(INFO) << "## sps_subpic_same_size_flag : " << (sps.sps_subpic_same_size_flag ? "1" : "0");
        DLOG(INFO) << "## sps_subpic_id_mapping_explicitly_signalled_flag : " << (sps.sps_subpic_id_mapping_explicitly_signalled_flag ? "1" : "0");
        DLOG(INFO) << "## sps_subpic_id_mapping_present_flag : " << (sps.sps_subpic_id_mapping_present_flag ? "1" : "0");
        
        for (size_t i = 0; i < sps.sps_subpic_id.size(); ++i) {
            DLOG(INFO) << "## sps_subpic_id[" << i << "] : " << sps.sps_subpic_id[i];
        }
    }
    
    // Bit depth and coding parameters
    DLOG(INFO) << "## sps_bitdepth_minus8 : " << sps.sps_bitdepth_minus8;
    DLOG(INFO) << "## sps_entropy_coding_sync_enabled_flag : " << (sps.sps_entropy_coding_sync_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_entry_point_offsets_present_flag : " << (sps.sps_entry_point_offsets_present_flag ? "1" : "0");
    DLOG(INFO) << "## sps_log2_max_pic_order_cnt_lsb_minus4 : " << sps.sps_log2_max_pic_order_cnt_lsb_minus4;
    DLOG(INFO) << "## sps_poc_msb_cycle_flag : " << (sps.sps_poc_msb_cycle_flag ? "1" : "0");
    if (sps.sps_poc_msb_cycle_flag) {
        DLOG(INFO) << "## sps_poc_msb_cycle_len_minus1 : " << sps.sps_poc_msb_cycle_len_minus1;
    }
    
    DLOG(INFO) << "## sps_num_extra_ph_bytes : " << sps.sps_num_extra_ph_bytes;
    DLOG(INFO) << "## sps_num_extra_sh_bytes : " << sps.sps_num_extra_sh_bytes;
    DLOG(INFO) << "## sps_sublayer_dpb_params_flag : " << (sps.sps_sublayer_dpb_params_flag ? "1" : "0");
    
    // Coding block parameters
    DLOG(INFO) << "## sps_log2_min_luma_coding_block_size_minus2 : " << sps.sps_log2_min_luma_coding_block_size_minus2;
    DLOG(INFO) << "## sps_partition_constraints_override_enabled_flag : " << (sps.sps_partition_constraints_override_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_intra_slice_luma : " << sps.sps_log2_diff_min_qt_min_cb_intra_slice_luma;
    DLOG(INFO) << "## sps_max_mtt_hierarchy_depth_intra_slice_luma : " << sps.sps_max_mtt_hierarchy_depth_intra_slice_luma;
    DLOG(INFO) << "## sps_log2_diff_max_bt_min_qt_intra_slice_luma : " << sps.sps_log2_diff_max_bt_min_qt_intra_slice_luma;
    DLOG(INFO) << "## sps_log2_diff_max_tt_min_qt_intra_slice_luma : " << sps.sps_log2_diff_max_tt_min_qt_intra_slice_luma;
    
    // Chroma parameters
    DLOG(INFO) << "## sps_qtbtt_dual_tree_intra_flag : " << (sps.sps_qtbtt_dual_tree_intra_flag ? "1" : "0");
    if (sps.sps_qtbtt_dual_tree_intra_flag) {
        DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_intra_slice_chroma : " << sps.sps_log2_diff_min_qt_min_cb_intra_slice_chroma;
        DLOG(INFO) << "## sps_log2_diff_max_tt_min_qt_intra_slice_chroma : " << sps.sps_log2_diff_max_tt_min_qt_intra_slice_chroma;
        DLOG(INFO) << "## sps_max_mtt_hierarchy_depth_intra_slice_chroma : " << sps.sps_max_mtt_hierarchy_depth_intra_slice_chroma;
        DLOG(INFO) << "## sps_log2_diff_max_bt_min_qt_intra_slice_chroma : " << sps.sps_log2_diff_max_bt_min_qt_intra_slice_chroma;
    }
    
    // Inter slice parameters
    DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_inter_slice : " << sps.sps_log2_diff_min_qt_min_cb_inter_slice;
    DLOG(INFO) << "## sps_max_mtt_hierarchy_depth_inter_slice : " << sps.sps_max_mtt_hierarchy_depth_inter_slice;
    DLOG(INFO) << "## sps_log2_diff_max_bt_min_qt_inter_slice : " << sps.sps_log2_diff_max_bt_min_qt_inter_slice;
    DLOG(INFO) << "## sps_log2_diff_max_tt_min_qt_inter_slice : " << sps.sps_log2_diff_max_tt_min_qt_inter_slice;
    
    // Transform parameters
    DLOG(INFO) << "## sps_max_luma_transform_size_64_flag : " << (sps.sps_max_luma_transform_size_64_flag ? "1" : "0");
    DLOG(INFO) << "## sps_transform_skip_enabled_flag : " << (sps.sps_transform_skip_enabled_flag ? "1" : "0");
    if (sps.sps_transform_skip_enabled_flag) {
        DLOG(INFO) << "## sps_log2_transform_skip_max_size_minus2 : " << sps.sps_log2_transform_skip_max_size_minus2;
    }
    
    DLOG(INFO) << "## sps_bdpcm_enabled_flag : " << (sps.sps_bdpcm_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_mts_enabled_flag : " << (sps.sps_mts_enabled_flag ? "1" : "0");
    if (sps.sps_mts_enabled_flag) {
        DLOG(INFO) << "## sps_explicit_mts_intra_enabled_flag : " << (sps.sps_explicit_mts_intra_enabled_flag ? "1" : "0");
        DLOG(INFO) << "## sps_explicit_mts_inter_enabled_flag : " << (sps.sps_explicit_mts_inter_enabled_flag ? "1" : "0");
    }
    
    DLOG(INFO) << "## sps_lfnst_enabled_flag : " << (sps.sps_lfnst_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_joint_cbcr_enabled_flag : " << (sps.sps_joint_cbcr_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_same_qp_table_for_chroma_flag : " << (sps.sps_same_qp_table_for_chroma_flag ? "1" : "0");
    
    // Filter parameters
    DLOG(INFO) << "## sps_sao_enabled_flag : " << (sps.sps_sao_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_ccalf_enabled_flag : " << (sps.sps_ccalf_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_alf_enabled_flag : " << (sps.sps_alf_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_lmcs_enabled_flag : " << (sps.sps_lmcs_enabled_flag ? "1" : "0");
    
    // Prediction parameters
    DLOG(INFO) << "## sps_weighted_pred_flag : " << (sps.sps_weighted_pred_flag ? "1" : "0");
    DLOG(INFO) << "## sps_weighted_bipred_flag : " << (sps.sps_weighted_bipred_flag ? "1" : "0");
    DLOG(INFO) << "## sps_long_term_ref_pics_flag : " << (sps.sps_long_term_ref_pics_flag ? "1" : "0");
    DLOG(INFO) << "## sps_inter_layer_prediction_enabled_flag : " << (sps.sps_inter_layer_prediction_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_idr_rpl_present_flag : " << (sps.sps_idr_rpl_present_flag ? "1" : "0");
    DLOG(INFO) << "## sps_rpl1_same_as_rpl0_flag : " << (sps.sps_rpl1_same_as_rpl0_flag ? "1" : "0");
    
    // Motion parameters
    DLOG(INFO) << "## sps_ref_wraparound_enabled_flag : " << (sps.sps_ref_wraparound_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_temporal_mvp_enabled_flag : " << (sps.sps_temporal_mvp_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_sbtmvp_enabled_flag : " << (sps.sps_sbtmvp_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_amvr_enabled_flag : " << (sps.sps_amvr_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_bdof_enabled_flag : " << (sps.sps_bdof_enabled_flag ? "1" : "0");
    if (sps.sps_bdof_enabled_flag) {
        DLOG(INFO) << "## sps_bdof_control_present_in_ph_flag : " << (sps.sps_bdof_control_present_in_ph_flag ? "1" : "0");
    }
    
    DLOG(INFO) << "## sps_smvd_enabled_flag : " << (sps.sps_smvd_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_dmvr_enabled_flag : " << (sps.sps_dmvr_enabled_flag ? "1" : "0");
    if (sps.sps_dmvr_enabled_flag) {
        DLOG(INFO) << "## sps_dmvr_control_present_in_ph_flag : " << (sps.sps_dmvr_control_present_in_ph_flag ? "1" : "0");
    }
    
    DLOG(INFO) << "## sps_mmvd_enabled_flag : " << (sps.sps_mmvd_enabled_flag ? "1" : "0");
    if (sps.sps_mmvd_enabled_flag) {
        DLOG(INFO) << "## sps_mmvd_fullpel_only_enabled_flag : " << (sps.sps_mmvd_fullpel_only_enabled_flag ? "1" : "0");
    }
    
    DLOG(INFO) << "## sps_six_minus_max_num_merge_cand : " << sps.sps_six_minus_max_num_merge_cand;
    DLOG(INFO) << "## sps_sbt_enabled_flag : " << (sps.sps_sbt_enabled_flag ? "1" : "0");
    
    // Affine parameters
    DLOG(INFO) << "## sps_affine_enabled_flag : " << (sps.sps_affine_enabled_flag ? "1" : "0");
    if (sps.sps_affine_enabled_flag) {
        DLOG(INFO) << "## sps_five_minus_max_num_subblock_merge_cand : " << sps.sps_five_minus_max_num_subblock_merge_cand;
        DLOG(INFO) << "## sps_6param_affine_enabled_flag : " << (sps.sps_6param_affine_enabled_flag ? "1" : "0");
        DLOG(INFO) << "## sps_affine_amvr_enabled_flag : " << (sps.sps_affine_amvr_enabled_flag ? "1" : "0");
        DLOG(INFO) << "## sps_affine_prof_enabled_flag : " << (sps.sps_affine_prof_enabled_flag ? "1" : "0");
        if (sps.sps_affine_prof_enabled_flag) {
            DLOG(INFO) << "## sps_prof_control_present_in_ph_flag : " << (sps.sps_prof_control_present_in_ph_flag ? "1" : "0");
        }
    }
    
    DLOG(INFO) << "## sps_bcw_enabled_flag : " << (sps.sps_bcw_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_ciip_enabled_flag : " << (sps.sps_ciip_enabled_flag ? "1" : "0");
    
    // GPM parameters
    DLOG(INFO) << "## sps_gpm_enabled_flag : " << (sps.sps_gpm_enabled_flag ? "1" : "0");
    if (sps.sps_gpm_enabled_flag) {
        DLOG(INFO) << "## sps_max_num_merge_cand_minus_max_num_gpm_cand : " << sps.sps_max_num_merge_cand_minus_max_num_gpm_cand;
    }
    
    DLOG(INFO) << "## sps_log2_parallel_merge_level_minus2 : " << sps.sps_log2_parallel_merge_level_minus2;
    
    // Intra prediction parameters
    DLOG(INFO) << "## sps_isp_enabled_flag : " << (sps.sps_isp_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_mrl_enabled_flag : " << (sps.sps_mrl_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_mip_enabled_flag : " << (sps.sps_mip_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_cclm_enabled_flag : " << (sps.sps_cclm_enabled_flag ? "1" : "0");
    if (sps.sps_cclm_enabled_flag) {
        DLOG(INFO) << "## sps_chroma_horizontal_collocated_flag : " << (sps.sps_chroma_horizontal_collocated_flag ? "1" : "0");
        DLOG(INFO) << "## sps_chroma_vertical_collocated_flag : " << (sps.sps_chroma_vertical_collocated_flag ? "1" : "0");
    }
    
    // Palette and other parameters
    DLOG(INFO) << "## sps_palette_enabled_flag : " << (sps.sps_palette_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_act_enabled_flag : " << (sps.sps_act_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_min_qp_prime_ts : " << sps.sps_min_qp_prime_ts;
    
    // IBC parameters
    DLOG(INFO) << "## sps_ibc_enabled_flag : " << (sps.sps_ibc_enabled_flag ? "1" : "0");
    if (sps.sps_ibc_enabled_flag) {
        DLOG(INFO) << "## sps_six_minus_max_num_ibc_merge_cand : " << sps.sps_six_minus_max_num_ibc_merge_cand;
    }
    
    // LADF parameters
    DLOG(INFO) << "## sps_ladf_enabled_flag : " << (sps.sps_ladf_enabled_flag ? "1" : "0");
    if (sps.sps_ladf_enabled_flag) {
        DLOG(INFO) << "## sps_num_ladf_intervals_minus2 : " << sps.sps_num_ladf_intervals_minus2;
        DLOG(INFO) << "## sps_ladf_lowest_interval_qp_offset : " << sps.sps_ladf_lowest_interval_qp_offset;
        for (size_t i = 0; i < sps.sps_ladf_qp_offset.size(); ++i) {
            DLOG(INFO) << "## sps_ladf_qp_offset[" << i << "] : " << sps.sps_ladf_qp_offset[i];
            DLOG(INFO) << "## sps_ladf_delta_threshold_minus1[" << i << "] : " << sps.sps_ladf_delta_threshold_minus1[i];
        }
    }
    
    // Scaling list parameters
    DLOG(INFO) << "## sps_explicit_scaling_list_enabled_flag : " << (sps.sps_explicit_scaling_list_enabled_flag ? "1" : "0");
    if (sps.sps_explicit_scaling_list_enabled_flag) {
        DLOG(INFO) << "## sps_scaling_matrix_for_lfnst_disabled_flag : " << (sps.sps_scaling_matrix_for_lfnst_disabled_flag ? "1" : "0");
        DLOG(INFO) << "## sps_scaling_matrix_for_alternative_colour_space_disabled_flag : " << (sps.sps_scaling_matrix_for_alternative_colour_space_disabled_flag ? "1" : "0");
        DLOG(INFO) << "## sps_scaling_matrix_designated_colour_space_flag : " << (sps.sps_scaling_matrix_designated_colour_space_flag ? "1" : "0");
    }
    
    // Quantization parameters
    DLOG(INFO) << "## sps_dep_quant_enabled_flag : " << (sps.sps_dep_quant_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## sps_sign_data_hiding_enabled_flag : " << (sps.sps_sign_data_hiding_enabled_flag ? "1" : "0");
    
    // Virtual boundaries
    DLOG(INFO) << "## sps_virtual_boundaries_enabled_flag : " << (sps.sps_virtual_boundaries_enabled_flag ? "1" : "0");
    if (sps.sps_virtual_boundaries_enabled_flag) {
        DLOG(INFO) << "## sps_virtual_boundaries_present_flag : " << (sps.sps_virtual_boundaries_present_flag ? "1" : "0");
        if (sps.sps_virtual_boundaries_present_flag) {
            DLOG(INFO) << "## sps_num_ver_virtual_boundaries : " << sps.sps_num_ver_virtual_boundaries;
            for (int i = 0; i < sps.sps_num_ver_virtual_boundaries; ++i) {
                DLOG(INFO) << "## sps_virtual_boundary_pos_x_minus1[" << i << "] : " << sps.sps_virtual_boundary_pos_x_minus1[i];
            }
            DLOG(INFO) << "## sps_num_hor_virtual_boundaries : " << sps.sps_num_hor_virtual_boundaries;
            for (int i = 0; i < sps.sps_num_hor_virtual_boundaries; ++i) {
                DLOG(INFO) << "## sps_virtual_boundary_pos_y_minus1[" << i << "] : " << sps.sps_virtual_boundary_pos_y_minus1[i];
            }
        }
    }
    
    // Timing and HRD
    DLOG(INFO) << "## sps_timing_hrd_params_present_flag : " << (sps.sps_timing_hrd_params_present_flag ? "1" : "0");
    
    DLOG(INFO) << "=== End of H.266 SPS Parameters ===";
}
/* ####################################################################################################################################*/
void DisplayH266PPS(const H266Pps& pps) {
    DLOG(INFO) << "=== H.266 PPS Parameters ===";
    
    // Basic parameters
    DLOG(INFO) << "## pic_parameter_set_id : " << pps.pic_parameter_set_id;
    DLOG(INFO) << "## seq_parameter_set_id : " << pps.seq_parameter_set_id;
    DLOG(INFO) << "## pps_pic_parameter_set_id : " << pps.pps_pic_parameter_set_id;
    DLOG(INFO) << "## pps_seq_parameter_set_id : " << pps.pps_seq_parameter_set_id;
    
    // Picture type and size
    DLOG(INFO) << "## pps_mixed_nalu_types_in_pic_flag : " << (pps.pps_mixed_nalu_types_in_pic_flag ? "1" : "0");
    DLOG(INFO) << "## pps_pic_width_in_luma_samples : " << pps.pps_pic_width_in_luma_samples;
    DLOG(INFO) << "## pps_pic_height_in_luma_samples : " << pps.pps_pic_height_in_luma_samples;
    
    // Conformance window
    DLOG(INFO) << "## pps_conformance_window_flag : " << (pps.pps_conformance_window_flag ? "1" : "0");
    if (pps.pps_conformance_window_flag) {
        DLOG(INFO) << "## pps_conf_win_left_offset : " << pps.pps_conf_win_left_offset;
        DLOG(INFO) << "## pps_conf_win_right_offset : " << pps.pps_conf_win_right_offset;
        DLOG(INFO) << "## pps_conf_win_top_offset : " << pps.pps_conf_win_top_offset;
        DLOG(INFO) << "## pps_conf_win_bottom_offset : " << pps.pps_conf_win_bottom_offset;
    }
    
    // Scaling window
    DLOG(INFO) << "## pps_scaling_window_explicit_signalling_flag : " << (pps.pps_scaling_window_explicit_signalling_flag ? "1" : "0");
    if (pps.pps_scaling_window_explicit_signalling_flag) {
        DLOG(INFO) << "## pps_scaling_win_left_offset : " << pps.pps_scaling_win_left_offset;
        DLOG(INFO) << "## pps_scaling_win_right_offset : " << pps.pps_scaling_win_right_offset;
        DLOG(INFO) << "## pps_scaling_win_top_offset : " << pps.pps_scaling_win_top_offset;
        DLOG(INFO) << "## pps_scaling_win_bottom_offset : " << pps.pps_scaling_win_bottom_offset;
    }
    
    // Output and partition flags
    DLOG(INFO) << "## pps_output_flag_present_flag : " << (pps.pps_output_flag_present_flag ? "1" : "0");
    DLOG(INFO) << "## pps_no_pic_partition_flag : " << (pps.pps_no_pic_partition_flag ? "1" : "0");
    
    // Subpicture parameters
    DLOG(INFO) << "## pps_subpic_id_mapping_present_flag : " << (pps.pps_subpic_id_mapping_present_flag ? "1" : "0");
    if (pps.pps_subpic_id_mapping_present_flag) {
        DLOG(INFO) << "## pps_num_subpics_minus1 : " << pps.pps_num_subpics_minus1;
        DLOG(INFO) << "## pps_subpic_id_len_minus1 : " << pps.pps_subpic_id_len_minus1;
        for (size_t i = 0; i < pps.pps_subpic_id.size(); ++i) {
            DLOG(INFO) << "## pps_subpic_id[" << i << "] : " << pps.pps_subpic_id[i];
        }
    }
    
    // CTU and tile parameters
    DLOG(INFO) << "## pps_log2_ctu_size_minus5 : " << pps.pps_log2_ctu_size_minus5;
    DLOG(INFO) << "## CtbSizeY : " << pps.CtbSizeY;
    
    if (!pps.pps_no_pic_partition_flag) {
        DLOG(INFO) << "## pps_num_exp_tile_columns_minus1 : " << pps.pps_num_exp_tile_columns_minus1;
        DLOG(INFO) << "## pps_num_exp_tile_rows_minus1 : " << pps.pps_num_exp_tile_rows_minus1;
        
        for (size_t i = 0; i < pps.pps_tile_column_width_minus1.size(); ++i) {
            DLOG(INFO) << "## pps_tile_column_width_minus1[" << i << "] : " << pps.pps_tile_column_width_minus1[i];
        }
        
        for (size_t i = 0; i < pps.pps_tile_row_height_minus1.size(); ++i) {
            DLOG(INFO) << "## pps_tile_row_height_minus1[" << i << "] : " << pps.pps_tile_row_height_minus1[i];
        }
        
        DLOG(INFO) << "## pps_loop_filter_across_tiles_enabled_flag : " << (pps.pps_loop_filter_across_tiles_enabled_flag ? "1" : "0");
        
        // Slice parameters
        DLOG(INFO) << "## pps_rect_slice_flag : " << (pps.pps_rect_slice_flag ? "1" : "0");
        if (pps.pps_rect_slice_flag) {
            DLOG(INFO) << "## pps_single_slice_per_subpic_flag : " << (pps.pps_single_slice_per_subpic_flag ? "1" : "0");
            if (!pps.pps_single_slice_per_subpic_flag) {
                DLOG(INFO) << "## pps_num_slices_in_pic_minus1 : " << pps.pps_num_slices_in_pic_minus1;
                DLOG(INFO) << "## pps_tile_idx_delta_present_flag : " << (pps.pps_tile_idx_delta_present_flag ? "1" : "0");
                
                for (size_t i = 0; i < pps.pps_slice_width_in_tiles_minus1.size(); ++i) {
                    DLOG(INFO) << "## pps_slice_width_in_tiles_minus1[" << i << "] : " << pps.pps_slice_width_in_tiles_minus1[i];
                }
                
                for (size_t i = 0; i < pps.pps_slice_height_in_tiles_minus1.size(); ++i) {
                    DLOG(INFO) << "## pps_slice_height_in_tiles_minus1[" << i << "] : " << pps.pps_slice_height_in_tiles_minus1[i];
                }
                
                for (size_t i = 0; i < pps.pps_num_exp_slices_in_tile.size(); ++i) {
                    DLOG(INFO) << "## pps_num_exp_slices_in_tile[" << i << "] : " << pps.pps_num_exp_slices_in_tile[i];
                }
                
                for (size_t i = 0; i < pps.pps_exp_slice_height_in_ctus_minus1.size(); ++i) {
                    for (size_t j = 0; j < pps.pps_exp_slice_height_in_ctus_minus1[i].size(); ++j) {
                        DLOG(INFO) << "## pps_exp_slice_height_in_ctus_minus1[" << i << "][" << j << "] : " << pps.pps_exp_slice_height_in_ctus_minus1[i][j];
                    }
                }
                
                for (size_t i = 0; i < pps.pps_tile_idx_delta_val.size(); ++i) {
                    DLOG(INFO) << "## pps_tile_idx_delta_val[" << i << "] : " << pps.pps_tile_idx_delta_val[i];
                }
            }
        }
        
        DLOG(INFO) << "## pps_loop_filter_across_slices_enabled_flag : " << (pps.pps_loop_filter_across_slices_enabled_flag ? "1" : "0");
    }
    
    // CABAC and reference picture parameters
    DLOG(INFO) << "## pps_cabac_init_present_flag : " << (pps.pps_cabac_init_present_flag ? "1" : "0");
    for (size_t i = 0; i < pps.pps_num_ref_idx_default_active_minus1.size(); ++i) {
        DLOG(INFO) << "## pps_num_ref_idx_default_active_minus1[" << i << "] : " << pps.pps_num_ref_idx_default_active_minus1[i];
    }
    
    DLOG(INFO) << "## pps_rpl1_idx_present_flag : " << (pps.pps_rpl1_idx_present_flag ? "1" : "0");
    
    // Weighted prediction
    DLOG(INFO) << "## weighted_pred_flag : " << (pps.weighted_pred_flag ? "1" : "0");
    DLOG(INFO) << "## weighted_bipred_flag : " << (pps.weighted_bipred_flag ? "1" : "0");
    DLOG(INFO) << "## pps_weighted_pred_flag : " << (pps.pps_weighted_pred_flag ? "1" : "0");
    DLOG(INFO) << "## pps_weighted_bipred_flag : " << (pps.pps_weighted_bipred_flag ? "1" : "0");
    
    // Reference wraparound
    DLOG(INFO) << "## pps_ref_wraparound_enabled_flag : " << (pps.pps_ref_wraparound_enabled_flag ? "1" : "0");
    if (pps.pps_ref_wraparound_enabled_flag) {
        DLOG(INFO) << "## pps_pic_width_minus_wraparound_offset : " << pps.pps_pic_width_minus_wraparound_offset;
    }
    
    // QP parameters
    DLOG(INFO) << "## no_qp_delta_flag : " << (pps.no_qp_delta_flag ? "1" : "0");
    DLOG(INFO) << "## init_qp_minus26 : " << pps.init_qp_minus26;
    DLOG(INFO) << "## pps_init_qp_minus26 : " << pps.pps_init_qp_minus26;
    DLOG(INFO) << "## cu_qp_delta_enabled_flag : " << (pps.cu_qp_delta_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## pps_cu_qp_delta_enabled_flag : " << (pps.pps_cu_qp_delta_enabled_flag ? "1" : "0");
    
    // Chroma QP offsets
    DLOG(INFO) << "## pps_chroma_tool_offsets_present_flag : " << (pps.pps_chroma_tool_offsets_present_flag ? "1" : "0");
    if (pps.pps_chroma_tool_offsets_present_flag) {
        DLOG(INFO) << "## pps_cb_qp_offset : " << pps.pps_cb_qp_offset;
        DLOG(INFO) << "## pps_cr_qp_offset : " << pps.pps_cr_qp_offset;
        DLOG(INFO) << "## pps_joint_cbcr_qp_offset_present_flag : " << (pps.pps_joint_cbcr_qp_offset_present_flag ? "1" : "0");
        if (pps.pps_joint_cbcr_qp_offset_present_flag) {
            DLOG(INFO) << "## pps_joint_cbcr_qp_offset_value : " << pps.pps_joint_cbcr_qp_offset_value;
        }
        DLOG(INFO) << "## pps_slice_chroma_qp_offsets_present_flag : " << (pps.pps_slice_chroma_qp_offsets_present_flag ? "1" : "0");
        DLOG(INFO) << "## pps_cu_chroma_qp_offset_list_enabled_flag : " << (pps.pps_cu_chroma_qp_offset_list_enabled_flag ? "1" : "0");
    }
    
    DLOG(INFO) << "## cu_chroma_qp_offset_list_len_minus1 : " << pps.cu_chroma_qp_offset_list_len_minus1;
    DLOG(INFO) << "## pps_cu_chroma_qp_offset_list_len_minus1 : " << pps.pps_cu_chroma_qp_offset_list_len_minus1;
    DLOG(INFO) << "## pps_chroma_qp_offset_list_len_minus1 : " << pps.pps_chroma_qp_offset_list_len_minus1;
    
    // QP offset lists
    for (size_t i = 0; i < pps.pps_cb_qp_offset_list.size(); ++i) {
        DLOG(INFO) << "## pps_cb_qp_offset_list[" << i << "] : " << pps.pps_cb_qp_offset_list[i];
    }
    
    for (size_t i = 0; i < pps.pps_cr_qp_offset_list.size(); ++i) {
        DLOG(INFO) << "## pps_cr_qp_offset_list[" << i << "] : " << pps.pps_cr_qp_offset_list[i];
    }
    
    for (size_t i = 0; i < pps.pps_joint_cbcr_qp_offset_list.size(); ++i) {
        DLOG(INFO) << "## pps_joint_cbcr_qp_offset_list[" << i << "] : " << pps.pps_joint_cbcr_qp_offset_list[i];
    }
    
    for (size_t i = 0; i < pps.pps_qp_offset_list.size(); ++i) {
        DLOG(INFO) << "## pps_qp_offset_list[" << i << "] : " << pps.pps_qp_offset_list[i];
    }
    
    // Deblocking filter parameters
    DLOG(INFO) << "## deblocking_filter_override_enabled_flag : " << (pps.deblocking_filter_override_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## deblocking_filter_disabled_flag : " << (pps.deblocking_filter_disabled_flag ? "1" : "0");
    DLOG(INFO) << "## deblocking_filter_beta_offset_div2 : " << pps.deblocking_filter_beta_offset_div2;
    DLOG(INFO) << "## deblocking_filter_tc_offset_div2 : " << pps.deblocking_filter_tc_offset_div2;
    
    DLOG(INFO) << "## pps_deblocking_filter_control_present_flag : " << (pps.pps_deblocking_filter_control_present_flag ? "1" : "0");
    if (pps.pps_deblocking_filter_control_present_flag) {
        DLOG(INFO) << "## pps_deblocking_filter_override_enabled_flag : " << (pps.pps_deblocking_filter_override_enabled_flag ? "1" : "0");
        DLOG(INFO) << "## pps_deblocking_filter_disabled_flag : " << (pps.pps_deblocking_filter_disabled_flag ? "1" : "0");
        if (!pps.pps_deblocking_filter_disabled_flag) {
            DLOG(INFO) << "## pps_luma_beta_offset_div2 : " << pps.pps_luma_beta_offset_div2;
            DLOG(INFO) << "## pps_luma_tc_offset_div2 : " << pps.pps_luma_tc_offset_div2;
            DLOG(INFO) << "## pps_cb_beta_offset_div2 : " << pps.pps_cb_beta_offset_div2;
            DLOG(INFO) << "## pps_cb_tc_offset_div2 : " << pps.pps_cb_tc_offset_div2;
            DLOG(INFO) << "## pps_cr_beta_offset_div2 : " << pps.pps_cr_beta_offset_div2;
            DLOG(INFO) << "## pps_cr_tc_offset_div2 : " << pps.pps_cr_tc_offset_div2;
        }
    }
    
    // Picture header info flags
    DLOG(INFO) << "## rpl_info_in_ph_flag : " << (pps.rpl_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## dbf_info_in_ph_flag : " << (pps.dbf_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## pps_rpl_info_in_ph_flag : " << (pps.pps_rpl_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## pps_sao_info_in_ph_flag : " << (pps.pps_sao_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## pps_alf_info_in_ph_flag : " << (pps.pps_alf_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## pps_wp_info_in_ph_flag : " << (pps.pps_wp_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## pps_qp_delta_info_in_ph_flag : " << (pps.pps_qp_delta_info_in_ph_flag ? "1" : "0");
    DLOG(INFO) << "## pps_dbf_info_in_ph_flag : " << (pps.pps_dbf_info_in_ph_flag ? "1" : "0");
    
    // Other flags
    DLOG(INFO) << "## cross_component_prediction_enabled_flag : " << (pps.cross_component_prediction_enabled_flag ? "1" : "0");
    DLOG(INFO) << "## chroma_tool_offsets_present_flag : " << (pps.chroma_tool_offsets_present_flag ? "1" : "0");
    DLOG(INFO) << "## log2_sao_offset_scale_luma : " << pps.log2_sao_offset_scale_luma;
    DLOG(INFO) << "## log2_sao_offset_scale_chroma : " << pps.log2_sao_offset_scale_chroma;
    
    // Tiles (legacy fields)
    DLOG(INFO) << "## tiles_enabled_flag : " << (pps.tiles_enabled_flag ? "1" : "0");
    if (pps.tiles_enabled_flag) {
        DLOG(INFO) << "## uniform_tile_spacing_flag : " << (pps.uniform_tile_spacing_flag ? "1" : "0");
        DLOG(INFO) << "## num_tile_columns_minus1 : " << pps.num_tile_columns_minus1;
        DLOG(INFO) << "## num_tile_rows_minus1 : " << pps.num_tile_rows_minus1;
        
        for (size_t i = 0; i < pps.tile_column_width_minus1.size(); ++i) {
            DLOG(INFO) << "## tile_column_width_minus1[" << i << "] : " << pps.tile_column_width_minus1[i];
        }
        
        for (size_t i = 0; i < pps.tile_row_height_minus1.size(); ++i) {
            DLOG(INFO) << "## tile_row_height_minus1[" << i << "] : " << pps.tile_row_height_minus1[i];
        }
        
        DLOG(INFO) << "## loop_filter_across_tiles_enabled_flag : " << (pps.loop_filter_across_tiles_enabled_flag ? "1" : "0");
    }
    
    // Extension flags
    DLOG(INFO) << "## slice_header_extension_present_flag : " << (pps.slice_header_extension_present_flag ? "1" : "0");
    DLOG(INFO) << "## pps_picture_header_extension_present_flag : " << (pps.pps_picture_header_extension_present_flag ? "1" : "0");
    DLOG(INFO) << "## pps_slice_header_extension_present_flag : " << (pps.pps_slice_header_extension_present_flag ? "1" : "0");
    DLOG(INFO) << "## pps_extension_flag : " << (pps.pps_extension_flag ? "1" : "0");
    DLOG(INFO) << "## pps_extension_data_flags : " << (pps.pps_extension_data_flags ? "1" : "0");
    DLOG(INFO) << "## pps_extension_data_flag : " << (pps.pps_extension_data_flag ? "1" : "0");
    
    DLOG(INFO) << "=== End of H.266 PPS Parameters ===";
}

/* ####################################################################################################################################*/
void DisplayGeneralTimingHrdParameters(const GeneralTimingHrdParameters& hrd) {
    DLOG(INFO) << "=== H.266 General Timing HRD Parameters ===";
    
    // Timing parameters
    DLOG(INFO) << "## num_units_in_tick : " << hrd.num_units_in_tick;
    DLOG(INFO) << "## time_scale : " << hrd.time_scale;
    
    // HRD presence flags
    DLOG(INFO) << "## general_nal_hrd_params_present_flag : " << (hrd.general_nal_hrd_params_present_flag ? "1" : "0");
    DLOG(INFO) << "## general_vcl_hrd_params_present_flag : " << (hrd.general_vcl_hrd_params_present_flag ? "1" : "0");
    
    // Timing flags
    DLOG(INFO) << "## general_same_pic_timing_in_all_ols_flag : " << (hrd.general_same_pic_timing_in_all_ols_flag ? "1" : "0");
    DLOG(INFO) << "## general_du_hrd_params_present_flag : " << (hrd.general_du_hrd_params_present_flag ? "1" : "0");
    
    // DU parameters (only if decoding unit HRD params are present)
    if (hrd.general_du_hrd_params_present_flag) {
        DLOG(INFO) << "## tick_divisor_minus2 : " << static_cast<int>(hrd.tick_divisor_minus2);
    }
    
    // Scale parameters
    DLOG(INFO) << "## bit_rate_scale : " << static_cast<int>(hrd.bit_rate_scale);
    DLOG(INFO) << "## cpb_size_scale : " << static_cast<int>(hrd.cpb_size_scale);
    
    if (hrd.general_du_hrd_params_present_flag) {
        DLOG(INFO) << "## cpb_size_du_scale : " << static_cast<int>(hrd.cpb_size_du_scale);
    }
    
    // CPB count
    DLOG(INFO) << "## hrd_cpb_cnt_minus1 : " << hrd.hrd_cpb_cnt_minus1;
    
    DLOG(INFO) << "=== End of H.266 General Timing HRD Parameters ===";
}
/* ####################################################################################################################################*/
void DisplayH266VPS(const H266Vps& vps) {
    DLOG(INFO) << "=== H.266 VPS Parameters ===";
    
    // Basic parameters
    DLOG(INFO) << "## vps_video_parameter_set_id : " << vps.vps_video_parameter_set_id;
    DLOG(INFO) << "## vps_max_layers_minus1 : " << vps.vps_max_layers_minus1;
    DLOG(INFO) << "## vps_max_sublayers_minus1 : " << vps.vps_max_sublayers_minus1;
    
    // Default flags
    DLOG(INFO) << "## vps_default_ptl_dpb_hrd_max_tid_flag : " << (vps.vps_default_ptl_dpb_hrd_max_tid_flag ? "1" : "0");
    DLOG(INFO) << "## vps_all_independent_layers_flag : " << (vps.vps_all_independent_layers_flag ? "1" : "0");
    
    // Layer IDs
    for (size_t i = 0; i < vps.vps_layer_id.size(); ++i) {
        DLOG(INFO) << "## vps_layer_id[" << i << "] : " << static_cast<int>(vps.vps_layer_id[i]);
    }
    
    // Independent layer flags
    for (size_t i = 0; i < vps.vps_independent_layer_flag.size(); ++i) {
        DLOG(INFO) << "## vps_independent_layer_flag[" << i << "] : " << (vps.vps_independent_layer_flag[i] ? "1" : "0");
    }
    
    // Max TID ref present flags
    for (size_t i = 0; i < vps.vps_max_tid_ref_present_flag.size(); ++i) {
        DLOG(INFO) << "## vps_max_tid_ref_present_flag[" << i << "] : " << (vps.vps_max_tid_ref_present_flag[i] ? "1" : "0");
    }
    
    // Direct reference layer flags (2D)
    for (size_t i = 0; i < vps.vps_direct_ref_layer_flag.size(); ++i) {
        for (size_t j = 0; j < vps.vps_direct_ref_layer_flag[i].size(); ++j) {
            DLOG(INFO) << "## vps_direct_ref_layer_flag[" << i << "][" << j << "] : " << (vps.vps_direct_ref_layer_flag[i][j] ? "1" : "0");
        }
    }
    
    // Max TID inter-layer reference pictures (2D)
    for (size_t i = 0; i < vps.vps_max_tid_il_ref_pics_plus1.size(); ++i) {
        for (size_t j = 0; j < vps.vps_max_tid_il_ref_pics_plus1[i].size(); ++j) {
            DLOG(INFO) << "## vps_max_tid_il_ref_pics_plus1[" << i << "][" << j << "] : " << vps.vps_max_tid_il_ref_pics_plus1[i][j];
        }
    }
    
    // OLS (Output Layer Set) parameters
    DLOG(INFO) << "## vps_each_layer_is_an_ols_flag : " << (vps.vps_each_layer_is_an_ols_flag ? "1" : "0");
    
    if (!vps.vps_each_layer_is_an_ols_flag) {
        DLOG(INFO) << "## vps_ols_mode_idc : " << vps.vps_ols_mode_idc;
        
        if (vps.vps_ols_mode_idc == 2) {
            DLOG(INFO) << "## vps_num_output_layer_sets_minus2 : " << vps.vps_num_output_layer_sets_minus2;
            
            // OLS output layer flags (2D)
            for (size_t i = 0; i < vps.vps_ols_output_layer_flag.size(); ++i) {
                for (size_t j = 0; j < vps.vps_ols_output_layer_flag[i].size(); ++j) {
                    DLOG(INFO) << "## vps_ols_output_layer_flag[" << i << "][" << j << "] : " << (vps.vps_ols_output_layer_flag[i][j] ? "1" : "0");
                }
            }
        }
    }
    
    // PTL (Profile Tier Level) parameters
    DLOG(INFO) << "## vps_num_ptls_minus1 : " << vps.vps_num_ptls_minus1;
    
    for (size_t i = 0; i < vps.vps_pt_present_flag.size(); ++i) {
        DLOG(INFO) << "## vps_pt_present_flag[" << i << "] : " << (vps.vps_pt_present_flag[i] ? "1" : "0");
    }
    
    for (size_t i = 0; i < vps.vps_ptl_max_tid.size(); ++i) {
        DLOG(INFO) << "## vps_ptl_max_tid[" << i << "] : " << vps.vps_ptl_max_tid[i];
    }
    
    DLOG(INFO) << "## vps_ptl_alignment_zero_bit : " << (vps.vps_ptl_alignment_zero_bit ? "1" : "0");
    
    for (size_t i = 0; i < vps.vps_ols_ptl_idx.size(); ++i) {
        DLOG(INFO) << "## vps_ols_ptl_idx[" << i << "] : " << vps.vps_ols_ptl_idx[i];
    }
    
    // DPB (Decoded Picture Buffer) parameters
    DLOG(INFO) << "## vps_num_dpb_params_minus1 : " << vps.vps_num_dpb_params_minus1;
    DLOG(INFO) << "## vps_sublayer_dpb_params_present_flag : " << (vps.vps_sublayer_dpb_params_present_flag ? "1" : "0");
    
    for (size_t i = 0; i < vps.vps_dpb_max_tid.size(); ++i) {
        DLOG(INFO) << "## vps_dpb_max_tid[" << i << "] : " << vps.vps_dpb_max_tid[i];
    }
    
    for (size_t i = 0; i < vps.vps_ols_dpb_pic_width.size(); ++i) {
        DLOG(INFO) << "## vps_ols_dpb_pic_width[" << i << "] : " << vps.vps_ols_dpb_pic_width[i];
    }
    
    for (size_t i = 0; i < vps.vps_ols_dpb_pic_height.size(); ++i) {
        DLOG(INFO) << "## vps_ols_dpb_pic_height[" << i << "] : " << vps.vps_ols_dpb_pic_height[i];
    }
    
    for (size_t i = 0; i < vps.vps_ols_dpb_chroma_format.size(); ++i) {
        DLOG(INFO) << "## vps_ols_dpb_chroma_format[" << i << "] : " << vps.vps_ols_dpb_chroma_format[i];
    }
    
    for (size_t i = 0; i < vps.vps_ols_dpb_bitdepth_minus8.size(); ++i) {
        DLOG(INFO) << "## vps_ols_dpb_bitdepth_minus8[" << i << "] : " << vps.vps_ols_dpb_bitdepth_minus8[i];
    }
    
    for (size_t i = 0; i < vps.vps_ols_dpb_params_idx.size(); ++i) {
        DLOG(INFO) << "## vps_ols_dpb_params_idx[" << i << "] : " << vps.vps_ols_dpb_params_idx[i];
    }
    
    // Timing and HRD parameters
    DLOG(INFO) << "## vps_timing_hrd_params_present_flag : " << (vps.vps_timing_hrd_params_present_flag ? "1" : "0");
    
    if (vps.vps_timing_hrd_params_present_flag) {
        // general_timing_hrd_parameters() would be called here
        DLOG(INFO) << "## vps_sublayer_cpb_params_present_flag : " << (vps.vps_sublayer_cpb_params_present_flag ? "1" : "0");
        DLOG(INFO) << "## vps_num_ols_timing_hrd_params_minus1 : " << vps.vps_num_ols_timing_hrd_params_minus1;
        
        for (size_t i = 0; i < vps.vps_hrd_max_tid.size(); ++i) {
            DLOG(INFO) << "## vps_hrd_max_tid[" << i << "] : " << vps.vps_hrd_max_tid[i];
        }
        
        for (size_t i = 0; i < vps.vps_ols_timing_hrd_idx.size(); ++i) {
            DLOG(INFO) << "## vps_ols_timing_hrd_idx[" << i << "] : " << vps.vps_ols_timing_hrd_idx[i];
        }
    }
    
    // Extension flags
    DLOG(INFO) << "## vps_extension_flag : " << (vps.vps_extension_flag ? "1" : "0");
    DLOG(INFO) << "## vps_extension_data_flag : " << (vps.vps_extension_data_flag ? "1" : "0");
    
    DLOG(INFO) << "=== End of H.266 VPS Parameters ===";
}
/* ####################################################################################################################################*/

/* ####################################################################################################################################*/

/* ####################################################################################################################################*/

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
  LOG(INFO) << "Calculating Pic Size in CTBs for H.266 SPS";

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
#if 0
H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu,
                                                H266SliceHeader* slice_header) {
  LOG(INFO) << "Parsing H.266 Slice Header NALU";

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
    if (slice_header->slice_type == kVvcPSlice ||
        slice_header->slice_type == kVvcBSlice) {
      TRUE_OR_RETURN(br->ReadBool(&slice_header->slice_rpl_present_flag));
      
      if (slice_header->slice_rpl_present_flag) {
        TRUE_OR_RETURN(br->ReadUE(&slice_header->num_ref_idx_l0_active_minus1));
        if (slice_header->slice_type == kVvcBSlice) {
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
#else

int findSubpicIndex(int subpicId, const std::vector<int>& SubpicIdVal) {
    auto it = std::find(SubpicIdVal.begin(), SubpicIdVal.end(), subpicId);
    if (it != SubpicIdVal.end()) {
        return static_cast<int>(std::distance(SubpicIdVal.begin(), it));
    }
    return 0; 
}

void AddCtbsToSlice(std::vector<std::vector<int>>& CtbAddrInSlice,
                    std::vector<int>& NumCtusInSlice,
                    int PicWidthInCtbsY,
                    int sliceIdx, int startX, int stopX, int startY, int stopY) {
    
    // Use push_back as in the original algorithm
    for (int ctbY = startY; ctbY < stopY; ctbY++) {
        for (int ctbX = startX; ctbX < stopX; ctbX++) {
            int ctbAddr = ctbY * PicWidthInCtbsY + ctbX;
            CtbAddrInSlice[sliceIdx].push_back(ctbAddr);
            NumCtusInSlice[sliceIdx]++;
        }
    }
}

std::vector<u_int32_t> DeriveTileColumnBoundaries(int NumTileColumns, 
                                           const std::vector<u_int32_t>& ColWidthVal) {
    // Create boundary array with size NumTileColumns + 1
    std::vector<u_int32_t> TileColBdVal(NumTileColumns + 1);
    
    // Initialize first boundary to 0
    TileColBdVal[0] = 0;
    
    // Calculate subsequent boundaries
    for (int i = 0; i < NumTileColumns; i++) {
        TileColBdVal[i + 1] = TileColBdVal[i] + ColWidthVal[i];
    }
    
    return TileColBdVal;
}


H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu,
                                                H266SliceHeader* slice_header) {
  LOG(INFO) << "Parsing H.266 Slice Header NALU";
  //7.3.2.14 Slice layer RBSP syntax
  std::unique_ptr<H266Sps> sps(new H266Sps);
  std::unique_ptr<H266Pps> pps(new H266Pps);

  //need extract 
/*   sh_slice_type 
  sh_num_ref_idx_active_override_flag 
  sh_num_ref_idx_active_minus1
  to calcultate  NumRefIdxActive[  equation 139 page 157
  Weight Predic  func
 */




  DCHECK(nalu.is_video_slice());
  *slice_header = H266SliceHeader();

  // Parses whole element.
  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  bool tmp_sh_picture_header_in_slice_header_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_sh_picture_header_in_slice_header_flag));
  slice_header->sh_picture_header_in_slice_header_flag = tmp_sh_picture_header_in_slice_header_flag;
  if(tmp_sh_picture_header_in_slice_header_flag){
    slice_header->phs.emplace();
    //picture_header_structure( )
    //H266PictureHeaderStructure* phs
    ParsePictureHeaderStructure(nalu, &slice_header->phs.value());
  }
  if(sps->sps_subpic_info_present_flag ){
    int tmp_sh_subpic_id = 0;
    int len_sh_subpic_id = sps->sps_subpic_id_len_minus1 + 1;
    TRUE_OR_RETURN(br->ReadBit(len_sh_subpic_id,&tmp_sh_subpic_id));
    slice_header->sh_subpic_id = tmp_sh_subpic_id;
  }
  /******************************************************/
  // 6.5.1 NumSlicesInSubpic[
  // 7.4.8 CurrSubpicIdx CurrSubpicIdx is derived to be such that SubpicIdVal[ CurrSubpicIdx ] is equal to sh_subpic_id.
  // The variable NumTilesInPic is set equal to NumTileColumns * NumTileRows. P28
  // need some processing before continue 
  // page 60
  /*******************************************************/




  // need extern function 
  for( int i = 0; i <= sps->sps_num_subpics_minus1; i++ ){
    if( sps->sps_subpic_id_mapping_explicitly_signalled_flag ){
       int tmp_sub_picIdVal = pps->pps_subpic_id_mapping_present_flag ? pps->pps_subpic_id[ i ] : sps->sps_subpic_id[ i ];
      slice_header->SubpicIdVal.push_back(tmp_sub_picIdVal);
    }
    else{
      slice_header->SubpicIdVal.push_back(0);
    }
  }

  uint32_t NumTileColumns = pps->NumTileColumns; //where get it ?
  uint32_t NumTileRows = pps->NumTileRows; //where get it ?
  int CurrSubpicIdx = 0;

  int NumTilesInPic = NumTileColumns*NumTileRows;

  if(slice_header->sh_subpic_id){
        CurrSubpicIdx = findSubpicIndex(slice_header->sh_subpic_id,slice_header->SubpicIdVal);
  }else{
    CurrSubpicIdx = 0;
  }
/* 
  The lists NumSlicesInSubpic[ i ], SubpicLevelSliceIdx[ j ], and SubpicIdxForSlice[ j ], specifying the number of slices
in the i-th subpicture, the subpicture-level slice index of the slice with picture-level slice index j, and the subpicture index
of the slice with picture-level slice index j, respectively, are derived as follows:
PAGE 32
 */
 //PicWidthInCtbsY eq 64 page 118 
 slice_header->PicWidthInCtbsY = ceil( pps->pps_pic_width_in_luma_samples / pps->CtbSizeY );
 slice_header->PicHeightInCtbsY = ceil( pps->pps_pic_height_in_luma_samples / pps->CtbSizeY );
 // need populate CtbAddrInSlice  eq 22 pgae 32 ...
 /****************************************************************************************/
//todo AddCtbsToSlice func;                        ok
//NumCtusInSlice[]                                 0k
//slice_header->subpicHeightLessThanOneTileFlag[]
//slice_header->ctbToTileColIdx[]
//slice_header->SubpicHeightInTiles[]
//slice_header->SubpicWidthInTiles[]


//slice_header->TileColBdVal[]   need ColWidthVal[  ok 
//slice_header->TileRowBdVal[]  need RowHeightVal[ ok 
/***************************need ColWidthVal to compute TileColBdVal ************************* */
//6.5.1 CTB raster scanning, tile scanning, and subpicture scanning processes
//ColWidthVal  equqtion 14 Page 28
int local_NumTileColumns = 0;
int inc_i= 0;
int remainingWidthInCtbsY = slice_header->PicWidthInCtbsY;
for( int i = 0; i <= pps->pps_num_exp_tile_columns_minus1; i++ ) {
  int tmp_sum_width = pps->pps_tile_column_width_minus1[i] + 1;
  slice_header->ColWidthVal.push_back(tmp_sum_width);
  remainingWidthInCtbsY -= slice_header->ColWidthVal[i];
}
u_int32_t uniformTileColWidth = pps->pps_tile_column_width_minus1[pps->pps_num_exp_tile_columns_minus1] + 1;
while( remainingWidthInCtbsY >= uniformTileColWidth ) {
  // i????
  slice_header->ColWidthVal[ inc_i ] = uniformTileColWidth;
  remainingWidthInCtbsY -= uniformTileColWidth;
  inc_i++; //no sure
}
if( remainingWidthInCtbsY > 0 ){
  slice_header->ColWidthVal[ inc_i ] = remainingWidthInCtbsY;
  inc_i++; //no sure
}
local_NumTileColumns = inc_i; //use fom pps
if(pps->NumTileColumns != local_NumTileColumns ){
  LOG(INFO) << "NumTileColumns from is not equal NumTileColumns fron slice_header, need to investigate";
}

/***************************************************************************/
slice_header->TileColBdVal = DeriveTileColumnBoundaries(NumTileColumns, slice_header->ColWidthVal);

/*******************compute RowHeightVal[**********************************/
int remainingHeightInCtbsY = slice_header->PicHeightInCtbsY;
int inc_y = 0;
int local_NumTileRows = 0;
for( int j = 0; j <= pps->pps_num_exp_tile_rows_minus1; j++ ) {
  int tmp_sum_height = pps->pps_tile_row_height_minus1[j] + 1;
  slice_header->RowHeightVal.push_back(tmp_sum_height);
  remainingHeightInCtbsY -= slice_header->RowHeightVal[j];
}
u_int32_t uniformTileRowHeight = pps->pps_tile_row_height_minus1[ pps->pps_num_exp_tile_rows_minus1 ] + 1;
while( remainingHeightInCtbsY >= uniformTileRowHeight ) {
  slice_header->RowHeightVal[inc_y] = uniformTileRowHeight;
  inc_y++;
}
if( remainingHeightInCtbsY > 0 ){
  slice_header->RowHeightVal[inc_y] = remainingHeightInCtbsY;
}
local_NumTileRows = inc_y;
if(pps->NumTileRows != local_NumTileRows){
    LOG(INFO) << "NumTileRows from is not equal NumTileRows fron slice_header, need to investigate";

}
/*****************************************************/

slice_header->TileRowBdVal = DeriveTileColumnBoundaries(NumTileRows, slice_header->RowHeightVal);

/***************** subpicHeightLessThanOneTileFlag  equqtion 20 page 30 ************************************/
for( int i = 0; i <= sps->sps_num_subpics_minus1; i++ ) {
  int leftX = sps->sps_subpic_ctu_top_left_x[i];
  int rightX = leftX + sps->sps_subpic_width_minus1[i];
  slice_header->SubpicWidthInTiles[ i ] = ctbToTileColIdx[ rightX ] + 1 - ctbToTileColIdx[ leftX ];
  int topY = sps->sps_subpic_ctu_top_left_y[i];
  int bottomY = topY + sps->sps_subpic_height_minus1[i];
  slice_header->SubpicHeightInTiles[i] = ctbToTileRowIdx[bottomY] + 1 - ctbToTileRowIdx[ topY ];
  if( slice_header->SubpicHeightInTiles[ i ] == 1 &&
      sps->sps_subpic_height_minus1[i] + 1 < slice_header->RowHeightVal[ ctbToTileRowIdx[ topY ] ] ){
        slice_header->subpicHeightLessThanOneTileFlag[ i ] = true;
  }else {
            slice_header->subpicHeightLessThanOneTileFlag[ i ] = false;
  }
}



/************************************************************************************************************/



 if( pps->pps_single_slice_per_subpic_flag ) {
  if(!sps->sps_subpic_info_present_flag){
    for( int j = 0; j < NumTileRows; j++ ){
      for( int i = 0; i < NumTileColumns; i++ ){
        //AddCtbsToSlice( 0, TileColBdVal[ i ], TileColBdVal[ i + 1 ], TileRowBdVal[ j ],TileRowBdVal[ j + 1 ] );
        AddCtbsToSlice(slice_header->CtbAddrInSlice,
                                slice_header->NumCtusInSlice,
                                slice_header->PicWidthInCtbsY,
                                0, slice_header->TileColBdVal[i], slice_header->TileColBdVal[i+1], slice_header->TileRowBdVal[ j ],slice_header->TileRowBdVal[j+1]);
      }
    }
  } else{
    for( int i = 0; i <= sps->sps_num_subpics_minus1; i++ ) {
      //NumCtusInSlice[ i ] = 0
      if( subpicHeightLessThanOneTileFlag[ i ] ){ /* The slice consists of a set of CTU rows in a tile. */
        AddCtbsToSlice( i, sps->sps_subpic_ctu_top_left_x[i] ,
          sps->sps_subpic_ctu_top_left_x[ i ] + sps->sps_subpic_width_minus1[ i ] + 1,
          sps->sps_subpic_ctu_top_left_y[ i ],
          sps->sps_subpic_ctu_top_left_y[i] + sps->sps_subpic_height_minus1[ i ] + 1 );
      } else { /* The slice consists of a number of complete tiles covering a rectangular region. */
        int tileX = ctbToTileColIdx[ sps->sps_subpic_ctu_top_left_x[i] ];
        int tileY = ctbToTileRowIdx[ sps->sps_subpic_ctu_top_left_y[ i ] ];
        for( int j = 0; j < SubpicHeightInTiles[ i ]; j++ ){
          for( int k = 0; k < SubpicWidthInTiles[ i ]; k++ ){
            AddCtbsToSlice( i, slice_header->TileColBdVal[ tileX + k ], slice_header->TileColBdVal[ tileX + k + 1 ], slice_header->TileRowBdVal[ tileY + j ], slice_header->TileRowBdVal[ tileY + j + 1 ] );
          }
        }
      }
    }
  }
}else{
  int tileIdx = 0;
  for( int i = 0; i <= pps->pps_num_slices_in_pic_minus1; i++ ){
    slice_header->NumCtusInSlice.push_back(0);
  }
  for(int i = 0; i <= pps->pps_num_slices_in_pic_minus1; i++ ) {
    slice_header->SliceTopLeftTileIdx.push_back(tileIdx);
    int tileX = tileIdx % NumTileColumns;
    int tileY = tileIdx / NumTileColumns;
    if( i < pps->pps_num_slices_in_pic_minus1 ) {
      slice_header->sliceWidthInTiles[ i ] = pps->pps_slice_width_in_tiles_minus1[ i ] + 1;
      slice_header->sliceHeightInTiles[ i ] = pps->pps_slice_height_in_tiles_minus1[ i ] + 1;

    } else {
      slice_header->sliceWidthInTiles[i] = NumTileColumns - tileX;
      slice_header->sliceHeightInTiles[i] = NumTileRows - tileY;
      slice_header->NumSlicesInTile[i] = 1;
    }
    if( slice_header->sliceWidthInTiles[ i ] == 1 && slice_header->sliceHeightInTiles[ i ] == 1 ) {

      //eq 21 page 31   no sure to have need all variables for my issue
    }


  }




}





 /****************************************************************************************/

 //need populate CtbAddrInSlice 
int posX = 0;
int posY = 0;
for( int i = 0; i <= sps->sps_num_subpics_minus1; i++ ) {
  slice_header->NumSlicesInSubpic[i] = 0;
  for( int j = 0; j <= pps->pps_num_slices_in_pic_minus1; j++ ) {
    posX = slice_header->CtbAddrInSlice[ j ][ 0 ] % slice_header->PicWidthInCtbsY;
    posY = slice_header->CtbAddrInSlice[ j ][ 0 ] / slice_header->PicWidthInCtbsY;
    if( ( posX >= sps->sps_subpic_ctu_top_left_x[i] ) && 
    ( posX < sps->sps_subpic_ctu_top_left_x[i] + sps->sps_subpic_width_minus1[i] + 1 ) && 
    ( posY >= sps->sps_subpic_ctu_top_left_y[i] ) && 
    ( posY < sps->sps_subpic_ctu_top_left_y[i] + sps->sps_subpic_height_minus1[i] + 1 ) ) {
      slice_header->SubpicIdxForSlice[ j ] = i;
      slice_header->SubpicLevelSliceIdx[j] = slice_header->NumSlicesInSubpic[i];
      slice_header->NumSlicesInSubpic[i] += 1;
    }
  }
}

   


/*   if( (pps->pps_rect_slice_flag && NumSlicesInSubpic[ CurrSubpicIdx ] > 1 ) || ( !pps->pps_rect_slice_flag && NumTilesInPic > 1 ) ){

  } */




  slice_header->header_bit_size = nalu.payload_size() * 8 - br->NumBitsLeft();
  return kOk;
}


#endif


#if 0 
H266Parser::Result H266Parser::ParsePps(const Nalu& nalu, int* pps_id) {
  DCHECK_EQ(Nalu::H266_PPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 PPS NALU";

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *pps_id = -1;
  std::unique_ptr<H266Pps> pps(new H266Pps);

  // Parsing minimal des champs essentiels
  uint32_t temp_pps_id;
  TRUE_OR_RETURN(br->ReadBits(6, &temp_pps_id));
  pps->pic_parameter_set_id = static_cast<int>(temp_pps_id);

  uint32_t temp_sps_id;
  TRUE_OR_RETURN(br->ReadBits(4, &temp_sps_id));
  pps->seq_parameter_set_id = static_cast<int>(temp_sps_id);

  TRUE_OR_RETURN(br->ReadBool(&pps->pps_mixed_nalu_types_in_pic_flag));

  // Skip tous les champs complexes pour l'instant
  // Nous les implémenterons progressivement une fois que la base compile
  
  // Byte alignment
  OK_OR_RETURN(ByteAlignment(br));

  *pps_id = pps->pic_parameter_set_id;
  active_ppses_[*pps_id] = std::move(pps);

  return kOk;
}


#else 
//disable all parsesps to build it
H266Parser::Result H266Parser::ParsePps(const Nalu& nalu, int* pps_id) {
  DCHECK_EQ(Nalu::H266_PPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 PPS NALU";

  //warning  with }} for close

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;
  uint32_t NumTileColumns = 0;  
  uint32_t NumTileRows = 0;   
  uint32_t NumTilesInPic = 0; 

  // get from pps
  if(pps->NumTileColumns != 0){
    NumTileColumns = pps->NumTileColumns;
  }
  if(pps->NumTileRows != 0){
    NumTileRows = pps->NumTileRows;
  }
  if(pps->NumTilesInPic != 0){
    NumTilesInPic = pps->NumTilesInPic;
  }




  *pps_id = -1;
  std::unique_ptr<H266Pps> pps(new H266Pps);
  std::unique_ptr<H266Sps> sps(new H266Sps);

  //pic_parameter_set_rbsp( ) 7.3.2.5

  TRUE_OR_RETURN(br->ReadBits(6, &pps->pic_parameter_set_id));  // 6 bits 
  TRUE_OR_RETURN(br->ReadBits(4,&pps->seq_parameter_set_id));  // 4 bits
  TRUE_OR_RETURN(br->ReadBool(&pps->pps_mixed_nalu_types_in_pic_flag));
  TRUE_OR_RETURN(br->ReadUE(&pps->pps_pic_width_in_luma_samples));
  TRUE_OR_RETURN(br->ReadUE(&pps->pps_pic_height_in_luma_samples));
  TRUE_OR_RETURN(br->ReadBool(&pps->pps_conformance_window_flag));

  if(pps->pps_conformance_window_flag){
    TRUE_OR_RETURN(br->ReadUE(&pps->pps_conf_win_left_offset));
    TRUE_OR_RETURN((br->ReadUE(&pps->pps_conf_win_left_offset)));
    TRUE_OR_RETURN((br->ReadUE(&pps->pps_conf_win_top_offset)));
    TRUE_OR_RETURN((br->ReadUE(&pps->pps_conf_win_bottom_offset)));
  }
  TRUE_OR_RETURN((br->ReadBool(&pps->pps_scaling_window_explicit_signalling_flag)));
  if(pps->pps_scaling_window_explicit_signalling_flag){
    TRUE_OR_RETURN((br->ReadSE(&pps->pps_scaling_win_left_offset)));
    TRUE_OR_RETURN((br->ReadSE(&pps->pps_scaling_win_right_offset)));
    TRUE_OR_RETURN((br->ReadSE(&pps->pps_scaling_win_top_offset)));
    TRUE_OR_RETURN((br->ReadSE(&pps->pps_scaling_win_bottom_offset)));
  }
  TRUE_OR_RETURN(br->ReadBool(&pps->pps_output_flag_present_flag));
  TRUE_OR_RETURN(br->ReadBool(&pps->pps_no_pic_partition_flag));
  TRUE_OR_RETURN(br->ReadBool(&pps->pps_subpic_id_mapping_present_flag));
  if(pps->pps_subpic_id_mapping_present_flag){
    if(!pps->pps_no_pic_partition_flag){
      TRUE_OR_RETURN((br->ReadUE(&pps->pps_num_subpics_minus1)));
      NumTileColumns = 1;
      NumTileRows = 1;
    }
    
    TRUE_OR_RETURN((br->ReadUE(&pps->pps_num_subpics_minus1)));
    u_int tmp_pps_subpic_id = 0;
    for(int i = 0;i <= pps->pps_num_subpics_minus1;i++){
      
      TRUE_OR_RETURN(br->ReadBits(sps->sps_subpic_id_len_minus1, &tmp_pps_subpic_id));
      pps->pps_subpic_id.push_back(tmp_pps_subpic_id);
    }
  }
  if(!pps->pps_no_pic_partition_flag){
    TRUE_OR_RETURN(br->ReadBits(2,&pps->pps_log2_ctu_size_minus5));  // 2 bits
    TRUE_OR_RETURN(br->ReadUE(&pps->pps_num_exp_tile_columns_minus1));
    TRUE_OR_RETURN(br->ReadUE(&pps->pps_num_exp_tile_rows_minus1));
    u_int tmp_pps_tile_column_width_minus1 = 0;
    for( int i = 0; i <= pps->pps_num_exp_tile_columns_minus1; i++ ){
      TRUE_OR_RETURN(br->ReadUE(&tmp_pps_tile_column_width_minus1));
      pps->pps_tile_column_width_minus1.push_back(tmp_pps_tile_column_width_minus1);
    }
    u_int tmp_pps_tile_row_height_minus1 = 0;
    for( int i = 0; i <= pps->pps_num_exp_tile_rows_minus1; i++ ){
      TRUE_OR_RETURN(br->ReadUE(&tmp_pps_tile_row_height_minus1));
      pps->pps_tile_row_height_minus1.push_back(tmp_pps_tile_row_height_minus1);
    }
    // not sure
    int CtbSizeY = 1 << (pps->pps_log2_ctu_size_minus5 + 5);
    int PicWidthInCtbsY = ceil(pps->pps_pic_width_in_luma_samples / CtbSizeY);
    int PicHeightInCtbsY = ceil(pps->pps_pic_height_in_luma_samples / CtbSizeY);
    //bckp up forr sps  and slice_header
    pps->CtbSizeY = CtbSizeY;

    int remainingWidthInCtbsY = PicWidthInCtbsY;
    for (int i = 0; i <= pps->pps_num_exp_tile_columns_minus1; i++) {
        int tileColWidth = pps->pps_tile_column_width_minus1[i] + 1;
        remainingWidthInCtbsY -= tileColWidth;
    }

    if (remainingWidthInCtbsY > 0) {
        int lastColWidth = pps->pps_tile_column_width_minus1[pps->pps_num_exp_tile_columns_minus1] + 1;
        NumTileColumns = pps->pps_num_exp_tile_columns_minus1 + 1 + 
                         ceil(remainingWidthInCtbsY / (float)lastColWidth);
    } else {
        NumTileColumns = pps->pps_num_exp_tile_columns_minus1 + 1;
    }
    /************************************************** */


    int remainingHeightInCtbsY = PicHeightInCtbsY;
    for (int i = 0; i <= pps->pps_num_exp_tile_rows_minus1; i++) {
        int tileRowHeight = pps->pps_tile_row_height_minus1[i] + 1;
        remainingHeightInCtbsY -= tileRowHeight;
    }
    if (remainingHeightInCtbsY > 0) {
        int lastRowHeight = pps->pps_tile_row_height_minus1[pps->pps_num_exp_tile_rows_minus1] + 1;
        NumTileRows = pps->pps_num_exp_tile_rows_minus1 + 1 + 
                      ceil(remainingHeightInCtbsY / (float)lastRowHeight);
    } else {
        NumTileRows = pps->pps_num_exp_tile_rows_minus1 + 1;
    }

    //NumTilesInPic is set equal to NumTileColumns * NumTileRows.
    uint32_t NumTilesInPic = NumTileColumns * NumTileRows;
    /************************************************** */
    //backup for slice_header parsing

    pps->NumTileColumns = NumTileColumns;
    pps->NumTileRows = NumTileRows;
    pps->NumTilesInPic = NumTilesInPic;
    /************************************************** */

    if( NumTilesInPic > 1 ) {
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_loop_filter_across_tiles_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_rect_slice_flag));
    }
    if(pps->pps_single_slice_per_subpic_flag){
      TRUE_OR_RETURN(br->ReadBool(&pps->pps_subpic_id_mapping_present_flag));
    }
    if( pps->pps_rect_slice_flag && !pps->pps_single_slice_per_subpic_flag ) {
      TRUE_OR_RETURN(br->ReadUE(&pps->pps_num_slices_in_pic_minus1));
      if(pps->pps_num_slices_in_pic_minus1 > 1 ){
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_tile_idx_delta_present_flag));
      }
      // #### I don't know populate this variable     check slice header 
      std::vector<uint32_t> SliceTopLeftTileIdx; // I don't know populate this variable

      //int tmp_pps_slice_height_in_tiles_minus1 = 0;
              std::vector<int> RowHeightVal;
        int remainingHeightInCtbsY;
        int CtbLog2SizeY = sps->sps_log2_ctu_size_minus5 + 5;
        int CtbSizeY = 1 << CtbLog2SizeY;

        int PicHeightInCtbsY = ceil( pps->pps_pic_height_in_luma_samples / CtbSizeY );
        int jj;

        remainingHeightInCtbsY = PicHeightInCtbsY;
        for( int jj = 0; jj <= pps->pps_num_exp_tile_rows_minus1; jj++ ) {
          RowHeightVal[jj] = pps->pps_tile_row_height_minus1[jj] + 1;
          remainingHeightInCtbsY -= RowHeightVal[jj];
        }
        int uniformTileRowHeight = pps->pps_tile_row_height_minus1[ pps->pps_num_exp_tile_rows_minus1 ] + 1;
        while( remainingHeightInCtbsY >= uniformTileRowHeight ) {
          RowHeightVal[jj++] = uniformTileRowHeight;
          remainingHeightInCtbsY -= uniformTileRowHeight;
        }
        if( remainingHeightInCtbsY > 0 ){
            RowHeightVal[jj++] = remainingHeightInCtbsY;
        }
        int NumTileRows = jj;
      int tmp_pps_slice_width_in_tiles_minus1 = 0;
      int tmp_pps_slice_height_in_tiles_minus1 = 0;

      for( int i = 0; i < pps->pps_num_slices_in_pic_minus1; i++ ) {
        if( SliceTopLeftTileIdx[ i ] % NumTileColumns != NumTileColumns-1 ){
          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_slice_width_in_tiles_minus1));
          pps->pps_slice_width_in_tiles_minus1.push_back(tmp_pps_slice_width_in_tiles_minus1);  
        }
        if( static_cast<int>(SliceTopLeftTileIdx[ i ] / NumTileColumns) != NumTileRows-1 && ( pps->pps_tile_idx_delta_present_flag || static_cast<int>(SliceTopLeftTileIdx[ i ] % NumTileColumns) == 0 ) ){
          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_slice_height_in_tiles_minus1));
          pps->pps_slice_height_in_tiles_minus1.push_back(tmp_pps_slice_height_in_tiles_minus1);
        }

        u_int tmp_pps_num_exp_slices_in_tile = 0;
        if( pps->pps_slice_width_in_tiles_minus1[ i ] == 0 && pps->pps_slice_height_in_tiles_minus1[ i ] == 0 && RowHeightVal[ SliceTopLeftTileIdx[ i ] / NumTileColumns ] > 1 ) {
          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_num_exp_slices_in_tile));
          pps->pps_num_exp_slices_in_tile.push_back(tmp_pps_num_exp_slices_in_tile);
          // not sure how to populate this variable
          std::vector<uint32_t> NumSlicesInTile;
          NumSlicesInTile.assign(NumTilesInPic, 0);
          for (uint32_t sliceIdx = 0; sliceIdx < SliceTopLeftTileIdx.size(); sliceIdx++) {
            uint32_t tileIdx = SliceTopLeftTileIdx[sliceIdx];
            if (tileIdx < NumSlicesInTile.size()) {
              NumSlicesInTile[tileIdx]++;
            }
          }


          for( int j = 0; j < pps->pps_num_exp_slices_in_tile[ i ]; j++ ){
            u_int tmp_pps_exp_slice_height_in_ctus_minus1 = 0;
            TRUE_OR_RETURN(br->ReadUE(&tmp_pps_exp_slice_height_in_ctus_minus1));
            pps->pps_exp_slice_height_in_ctus_minus1[i][j] = tmp_pps_exp_slice_height_in_ctus_minus1;
            
            //i += NumSlicesInTile[i] -1;
            int numSlices = NumSlicesInTile[i];
            i += numSlices - 1;


          }
          if( pps->pps_tile_idx_delta_present_flag && i < pps->pps_num_slices_in_pic_minus1 ){
            int tmp_pps_tile_idx_delta_val = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_tile_idx_delta_val));
            pps->pps_tile_idx_delta_val.push_back(tmp_pps_tile_idx_delta_val);
          }

        }
        if( !pps->pps_rect_slice_flag || pps->pps_single_slice_per_subpic_flag || pps->pps_num_slices_in_pic_minus1 > 0 ){
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_loop_filter_across_slices_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_cabac_init_present_flag));
        int tmp_pps_num_ref_idx_default_active_minus1= 0;
        for( i = 0; i < 2; i++ ) {
          TRUE_OR_RETURN(br->ReadSE(&tmp_pps_num_ref_idx_default_active_minus1));
          pps->pps_num_ref_idx_default_active_minus1.push_back(tmp_pps_num_ref_idx_default_active_minus1);
        }
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_rpl1_idx_present_flag));
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_weighted_pred_flag));
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_weighted_bipred_flag));
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_ref_wraparound_enabled_flag));
        if( pps->pps_ref_wraparound_enabled_flag ) {
          TRUE_OR_RETURN(br->ReadUE(&pps->pps_pic_width_minus_wraparound_offset));
        }
        TRUE_OR_RETURN(br->ReadSE(&pps->pps_init_qp_minus26));
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_cu_qp_delta_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_chroma_tool_offsets_present_flag));
        if( pps->pps_chroma_tool_offsets_present_flag ) {
          TRUE_OR_RETURN(br->ReadSE(&pps->pps_cb_qp_offset));
          TRUE_OR_RETURN(br->ReadSE(&pps->pps_cr_qp_offset));
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_joint_cbcr_qp_offset_present_flag));
          if( pps->pps_joint_cbcr_qp_offset_present_flag ) {
            TRUE_OR_RETURN(br->ReadSE(&pps->pps_joint_cbcr_qp_offset_value));
          }
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_slice_chroma_qp_offsets_present_flag));
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_cu_chroma_qp_offset_list_enabled_flag));
          if( pps->pps_cu_chroma_qp_offset_list_enabled_flag ) {
            TRUE_OR_RETURN(br->ReadUE(&pps->pps_cu_chroma_qp_offset_list_len_minus1));
          }
          for( int i = 0; i <= pps->pps_chroma_qp_offset_list_len_minus1; i++ ){
            int tmp_pps_cb_qp_offset_list = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cb_qp_offset_list));
            pps->pps_cb_qp_offset_list.push_back(tmp_pps_cb_qp_offset_list);
            int tmp_pps_cr_qp_offset_list = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cr_qp_offset_list));
            pps->pps_cr_qp_offset_list.push_back(tmp_pps_cr_qp_offset_list);
            if( pps->pps_joint_cbcr_qp_offset_present_flag ) {
              int tmp_pps_joint_cbcr_qp_offset_list = 0;
              TRUE_OR_RETURN(br->ReadSE(&tmp_pps_joint_cbcr_qp_offset_list));
              pps->pps_joint_cbcr_qp_offset_list.push_back(tmp_pps_joint_cbcr_qp_offset_list);
            }
          }
        }
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_deblocking_filter_control_present_flag));
        if( pps->pps_deblocking_filter_control_present_flag ) {
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_deblocking_filter_override_enabled_flag));
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_deblocking_filter_disabled_flag));

          if( !pps->pps_no_pic_partition_flag && pps->pps_deblocking_filter_override_enabled_flag ) {
            TRUE_OR_RETURN(br->ReadBool(&pps->pps_dbf_info_in_ph_flag));
          }
          if( !pps->pps_deblocking_filter_disabled_flag ) {
            TRUE_OR_RETURN(br->ReadSE(&pps->pps_luma_beta_offset_div2));
            TRUE_OR_RETURN(br->ReadSE(&pps->pps_luma_tc_offset_div2));
            if( pps->chroma_tool_offsets_present_flag ) {
              TRUE_OR_RETURN(br->ReadSE(&pps->pps_cb_beta_offset_div2));
              TRUE_OR_RETURN(br->ReadSE(&pps->pps_cb_tc_offset_div2));
              TRUE_OR_RETURN(br->ReadSE(&pps->pps_cr_beta_offset_div2));
              TRUE_OR_RETURN(br->ReadSE(&pps->pps_cr_tc_offset_div2));
            }
        }
      }

      if( !pps->pps_no_pic_partition_flag ) {
      TRUE_OR_RETURN(br->ReadBool(&pps->pps_rpl_info_in_ph_flag));
      TRUE_OR_RETURN(br->ReadBool(&pps->pps_sao_info_in_ph_flag));
      TRUE_OR_RETURN(br->ReadBool(&pps->pps_alf_info_in_ph_flag));
      
        if( ( pps->pps_weighted_pred_flag || pps->pps_weighted_bipred_flag ) && pps->pps_rpl_info_in_ph_flag ){
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_wp_info_in_ph_flag));
         }
        TRUE_OR_RETURN(br->ReadBool(&pps->pps_qp_delta_info_in_ph_flag));
      }

      TRUE_OR_RETURN(br->ReadBool(&pps->pps_slice_header_extension_present_flag));
      TRUE_OR_RETURN(br->ReadBool(&pps->pps_extension_flag)); 

      if( pps->pps_extension_flag ) {
          while( br->more_rbsp_data() ) { 
              bool extension_data_flag;
              TRUE_OR_RETURN(br->ReadBool(&extension_data_flag));
              pps->pps_extension_data_flags = extension_data_flag;
          }
      }
      OK_OR_RETURN(ByteAlignment(br));    

        
    }
  }
}
   DisplayH266PPS(*pps);

  // This will replace any existing PPS instance.
  *pps_id = pps->pic_parameter_set_id;
  active_ppses_[*pps_id] = std::move(pps);

  return kOk;
}
#endif 
//H266Parser::Result H266Parser::ParsePps(const Nalu& nalu, int* pps_id) {

H266Parser::Result H266Parser::ParseSps(const Nalu& nalu, int* sps_id) {
  DCHECK_EQ(Nalu::H266_SPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 SPS NALU";
  //seq_parameter_set_rbsp( ) 7.3.2.4

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *sps_id = -1;
  std::unique_ptr<H266Sps> sps(new H266Sps);
  // GET from context ???
  std::unique_ptr<H266Pps> pps(new H266Pps);


  TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_seq_parameter_set_id));
  TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_video_parameter_set_id));
  TRUE_OR_RETURN(br->ReadBits(3, &sps->max_sublayers_minus1));
  TRUE_OR_RETURN(br->ReadBits(2, &sps->sps_chroma_format_idc));
  TRUE_OR_RETURN(br->ReadBits(2, &sps->sps_log2_ctu_size_minus5));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_ptl_dpb_hrd_params_present_flag));
  DLOG(INFO) << "## sps->sps_ptl_dpb_hrd_params_present_flag : " <<  ( sps->sps_ptl_dpb_hrd_params_present_flag ? "1" : "0");
  if( sps->sps_ptl_dpb_hrd_params_present_flag) {
    ParseProfileTierLevel(true, sps->max_sublayers_minus1, br, &sps->sps_profile_level);
  }
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_gdr_enabled_flag));
  DLOG(INFO) << "## sps->sps_gdr_enabled_flag : " << ( sps->sps_gdr_enabled_flag ? "1" : "0");
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_ref_pic_resampling_enabled_flag));
  DLOG(INFO) << "## sps_ref_pic_resampling_enabled_flag : " << ( sps->sps_ref_pic_resampling_enabled_flag ? "1" : "0");
  if( sps->sps_ref_pic_resampling_enabled_flag) {
    TRUE_OR_RETURN(br->ReadBool(&sps->sps_res_change_in_clvs_allowed_flag));
    DLOG(INFO) << "## sps->sps_ref_pic_resampling_enabled_flag : " << ( sps->sps_ref_pic_resampling_enabled_flag ? "1" : "0"); 
 }
  TRUE_OR_RETURN(br->ReadUE(&sps->sps_pic_width_max_in_luma_samples));
  DLOG(INFO) << "## sps->sps_pic_width_max_in_luma_samples : " << sps->sps_pic_width_max_in_luma_samples; 

  TRUE_OR_RETURN(br->ReadUE(&sps->sps_pic_height_max_in_luma_samples));
  DLOG(INFO) << "## sps->sps_pic_width_max_in_luma_samples : " << sps->sps_pic_height_max_in_luma_samples; 

  TRUE_OR_RETURN(br->ReadBool(&sps->sps_conformance_window_flag));
  if (sps->sps_conformance_window_flag) {
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_left_offset));
    DLOG(INFO) << "## sps->sps_conf_win_left_offset  : " << sps->sps_conf_win_left_offset;
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_right_offset));
    DLOG(INFO) << "##  : sps->sps_conf_win_right_offset " << sps->sps_conf_win_right_offset;
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_top_offset));
    DLOG(INFO) << "##  : sps->sps_conf_win_top_offset" << sps->sps_conf_win_top_offset;
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_bottom_offset));
    DLOG(INFO) << "## sps->sps_conf_win_bottom_offset : " << sps->sps_conf_win_bottom_offset;
  }
    TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_info_present_flag));
    DLOG(INFO) << "## sps->sps_subpic_info_present_flag : " << (sps->sps_subpic_info_present_flag  ? "1" : "0");
    if(sps->sps_subpic_info_present_flag) {
      TRUE_OR_RETURN(br->ReadUE(&sps->sps_num_subpics_minus1));
      DLOG(INFO) << "## sps->sps_num_subpics_minus1 : " << sps->sps_num_subpics_minus1;
    }
    if(sps->sps_num_subpics_minus1 > 0) {
      TRUE_OR_RETURN(br->ReadBool(&sps->sps_independent_subpics_flag));
      DLOG(INFO) << "## sps->sps_independent_subpics_flag : " << (sps->sps_independent_subpics_flag ? "1" : "0");
      TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_same_size_flag));
      DLOG(INFO) << "## sps->sps_subpic_same_size_flag : " << ( sps->sps_subpic_same_size_flag ? "1" : "0");
    }

    if(sps->sps_num_subpics_minus1 > 0){
        for(int i=0; i<= sps->sps_num_subpics_minus1 ; i++) {
          if(!sps->sps_subpic_same_size_flag && i>0) {
            // define CtbSizeY
            int CtbLog2SizeY = sps->sps_log2_ctu_size_minus5 + 5;
            int CtbSizeY = 1 << CtbLog2SizeY;
            int tmpWidthVal = ((sps->sps_pic_width_max_in_luma_samples + CtbSizeY-1 ) / CtbSizeY);
            int tmpHeightVal = (( sps->sps_pic_height_max_in_luma_samples + CtbSizeY-1 ) / CtbSizeY);
            // todo recheck this section    
            int bit_read = ceil(log2(tmpWidthVal));

            u_int tmp_sps_subpic_ctu_top_left_x = 0;
            if(i>0 && sps->sps_pic_width_max_in_luma_samples > CtbSizeY) {
              TRUE_OR_RETURN(br->ReadBits(bit_read,&tmp_sps_subpic_ctu_top_left_x));  // u(v)  NOT SURE
              sps->sps_subpic_ctu_top_left_x.push_back(tmp_sps_subpic_ctu_top_left_x);
              DLOG(INFO) << "## SPS->sps_subpic_ctu_top_left_x : " << tmp_sps_subpic_ctu_top_left_x;
            }
            int tmp_sps_subpic_ctu_top_left_y = 0;
            if( i > 0 && sps->sps_pic_height_max_in_luma_samples > CtbSizeY ){
              TRUE_OR_RETURN(br->ReadBits(tmpHeightVal,&tmp_sps_subpic_ctu_top_left_y));  // u(v)  NOT SURE
              sps->sps_subpic_ctu_top_left_y.push_back(tmp_sps_subpic_ctu_top_left_y);
              DLOG(INFO) << "## sps_subpic_ctu_top_left_y : " << tmp_sps_subpic_ctu_top_left_y;
            }
            int tmp_sps_subpic_width_minus1 = 0;
            if( i < sps->sps_num_subpics_minus1 && sps->sps_pic_width_max_in_luma_samples > CtbSizeY ){              
              TRUE_OR_RETURN(br->ReadBits(tmpWidthVal,&tmp_sps_subpic_width_minus1));  // u(v)  NOT SURE
              sps->sps_subpic_width_minus1.push_back(tmp_sps_subpic_width_minus1);
              DLOG(INFO) << "## sps_subpic_width_minus1 : " << tmp_sps_subpic_width_minus1;
            }
            int tmp_sps_subpic_height_minus1 = 0;
            if( i < sps->sps_num_subpics_minus1 && sps->sps_pic_height_max_in_luma_samples > CtbSizeY ){
              //sps_subpic_height_minus1[
              TRUE_OR_RETURN(br->ReadBits(tmpHeightVal,&tmp_sps_subpic_height_minus1));  // u(v)  NOT SURE
              sps->sps_subpic_ctu_top_left_y.push_back(tmp_sps_subpic_height_minus1);
              DLOG(INFO) << "## sps_subpic_height_minus1 : " << tmp_sps_subpic_height_minus1;
            }
              
          }
          if( !sps->sps_independent_subpics_flag) {
            bool tmp_sps_subpic_treated_as_pic_flag;
            bool tmp_sps_loop_filter_across_subpic_enabled_flag;

          TRUE_OR_RETURN(br->ReadBool(&tmp_sps_subpic_treated_as_pic_flag));
          sps->sps_subpic_treated_as_pic_flag.push_back(tmp_sps_subpic_treated_as_pic_flag);
          DLOG(INFO) << "## sps_subpic_treated_as_pic_flag : " << ( tmp_sps_subpic_treated_as_pic_flag ? "1" : "0");
          TRUE_OR_RETURN(br->ReadBool(&tmp_sps_loop_filter_across_subpic_enabled_flag));
          sps->sps_loop_filter_across_subpic_enabled_flag.push_back(tmp_sps_loop_filter_across_subpic_enabled_flag);
          DLOG(INFO) << "## tmp_sps_loop_filter_across_subpic_enabled_flag : " << ( tmp_sps_loop_filter_across_subpic_enabled_flag ? "1" : "0");
          }
        } //for 
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_subpic_id_len_minus1));
        DLOG(INFO) << "## sps->sps_subpic_id_len_minus1 : " << sps->sps_subpic_id_len_minus1;
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_id_mapping_explicitly_signalled_flag));
        DLOG(INFO) << "## sps->sps_subpic_id_mapping_explicitly_signalled_flag : " << ( sps->sps_subpic_id_mapping_explicitly_signalled_flag ? "1" : "0");

        TRUE_OR_RETURN(br->ReadBool(&sps->sps_entry_point_offsets_present_flag));
        DLOG(INFO) << "## sps->sps_entry_point_offsets_present_flag : " << ( sps->sps_entry_point_offsets_present_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_log2_max_pic_order_cnt_lsb_minus4));
        DLOG(INFO) << "## sps->sps_log2_max_pic_order_cnt_lsb_minus4 : " << sps->sps_log2_max_pic_order_cnt_lsb_minus4;
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_poc_msb_cycle_flag));
        DLOG(INFO) << "## sps->sps_poc_msb_cycle_flag : " << ( sps->sps_poc_msb_cycle_flag ? "1" : "0");



        if(sps->sps_poc_msb_cycle_flag){
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_poc_msb_cycle_len_minus1));
            DLOG(INFO) << "## sps->sps_poc_msb_cycle_len_minus1 : " << sps->sps_poc_msb_cycle_len_minus1;
        }
        TRUE_OR_RETURN(br->ReadBits(2,&sps->sps_num_extra_ph_bytes));
        DLOG(INFO) << "## sps->sps_num_extra_ph_bytes : " << sps->sps_num_extra_ph_bytes;
        bool tmp_sps_extra_sh_bit_present_flag = 0;
        for( int i = 0; i < (sps->sps_num_extra_sh_bytes * 8 ); i++ ){
          TRUE_OR_RETURN(br->ReadBool(&tmp_sps_extra_sh_bit_present_flag));
          sps->sps_extra_sh_bit_present_flag.push_back(tmp_sps_extra_sh_bit_present_flag);
          DLOG(INFO) << "## sps_extra_sh_bit_present_flag : " << ( tmp_sps_extra_sh_bit_present_flag ? "1" : "0");
        }
        if( sps->sps_ptl_dpb_hrd_params_present_flag ) {
          if( sps->max_sublayers_minus1 > 0 ){
              TRUE_OR_RETURN(br->ReadBool(&sps->sps_sublayer_dpb_params_flag));
              DLOG(INFO) << "## sps->sps_sublayer_dpb_params_flag : " << ( sps->sps_sublayer_dpb_params_flag ? "1" : "0");
              //TODO
             //dpb_parameters( sps_max_sublayers_minus1, sps_sublayer_dpb_params_flag )
          }
        }
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_min_luma_coding_block_size_minus2));
        DLOG(INFO) << "## sps->sps_log2_min_luma_coding_block_size_minus2 : " << sps->sps_log2_min_luma_coding_block_size_minus2;
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_partition_constraints_override_enabled_flag));
        DLOG(INFO) << "## sps->sps_partition_constraints_override_enabled_flag : " << ( sps->sps_partition_constraints_override_enabled_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma));
        DLOG(INFO) << "## sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma : " << sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma;
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_intra_slice_luma));
        DLOG(INFO) << "## sps->sps_max_mtt_hierarchy_depth_intra_slice_luma : " << sps->sps_max_mtt_hierarchy_depth_intra_slice_luma;
        if( sps->sps_max_mtt_hierarchy_depth_intra_slice_luma != 0 ) {
           TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma));
           DLOG(INFO) << "## sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma : " << sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma;
           TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma));
           DLOG(INFO) << "## sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma : " << sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma;
        }
        if( sps->sps_chroma_format_idc != 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_qtbtt_dual_tree_intra_flag));
          DLOG(INFO) << "## sps->sps_qtbtt_dual_tree_intra_flag : " << ( sps->sps_qtbtt_dual_tree_intra_flag ? "1" : "0");
        }
        if( sps->sps_qtbtt_dual_tree_intra_flag ) {

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma));
            DLOG(INFO) << "## sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma : " << sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma;
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma));
            DLOG(INFO) << "## sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma : " << sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma;
            if( sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma != 0 ) {
                TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma));
                DLOG(INFO) << "## sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma : " << sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma;
                TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma));
                DLOG(INFO) << "## sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma : " << sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma;
            }
        }


        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_inter_slice));
        DLOG(INFO) << "## sps->sps_log2_diff_min_qt_min_cb_inter_slice : " << sps->sps_log2_diff_min_qt_min_cb_inter_slice;
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_inter_slice));
        DLOG(INFO) << "## sps->sps_max_mtt_hierarchy_depth_inter_slice : " << sps->sps_max_mtt_hierarchy_depth_inter_slice;
        if( sps->sps_max_mtt_hierarchy_depth_inter_slice != 0 ) {
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_inter_slice));
          DLOG(INFO) << "## sps->sps_log2_diff_max_bt_min_qt_inter_slice : " << sps->sps_log2_diff_max_bt_min_qt_inter_slice;
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_inter_slice));
          DLOG(INFO) << "## sps->sps_log2_diff_max_tt_min_qt_inter_slice : " << sps->sps_log2_diff_max_tt_min_qt_inter_slice;
        }
        if( pps->CtbSizeY > 32 )
        {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_max_luma_transform_size_64_flag));
          DLOG(INFO) << "## sps->sps_max_luma_transform_size_64_flag : " << ( sps->sps_max_luma_transform_size_64_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_transform_skip_enabled_flag));
        DLOG(INFO) << "## sps->sps_transform_skip_enabled_flag : " << ( sps->sps_transform_skip_enabled_flag ? "1" : "0");
        if( sps->sps_transform_skip_enabled_flag ) {
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_transform_skip_max_size_minus2));
            DLOG(INFO) << "## sps->sps_log2_transform_skip_max_size_minus2 : " << sps->sps_log2_transform_skip_max_size_minus2;
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdpcm_enabled_flag));
            DLOG(INFO) << "## sps->sps_bdpcm_enabled_flag : " << ( sps->sps_bdpcm_enabled_flag ? "1" : "0");
            
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mts_enabled_flag));
        DLOG(INFO) << "## sps->sps_mts_enabled_flag : " << ( sps->sps_mts_enabled_flag ? "1" : "0");
        if( sps->sps_mts_enabled_flag ) {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_mts_intra_enabled_flag));
          DLOG(INFO) << "## sps->sps_explicit_mts_intra_enabled_flag : " << ( sps->sps_explicit_mts_intra_enabled_flag ? "1" : "0");
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_mts_inter_enabled_flag));
          DLOG(INFO) << "## sps->sps_explicit_mts_inter_enabled_flag : " << ( sps->sps_explicit_mts_inter_enabled_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_lfnst_enabled_flag));
        DLOG(INFO) << "## sps->sps_lfnst_enabled_flag : " << ( sps->sps_lfnst_enabled_flag ? "1" : "0");

        if( sps->sps_chroma_format_idc != 0 ) {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_joint_cbcr_enabled_flag));
          DLOG(INFO) << "## sps->sps_joint_cbcr_enabled_flag : " << ( sps->sps_joint_cbcr_enabled_flag ? "1" : "0");
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_same_qp_table_for_chroma_flag));
          DLOG(INFO) << "## sps->sps_same_qp_table_for_chroma_flag : " << ( sps->sps_same_qp_table_for_chroma_flag ? "1" : "0");
          int numQpTables = sps->sps_same_qp_table_for_chroma_flag ? 1 : ( sps->sps_joint_cbcr_enabled_flag ? 3 : 2 );
          int tmp_sps_qp_table_start_minus26 = 0;
          int tmp_sps_num_points_in_qp_table_minus1 = 0;
          for( int i = 0; i < numQpTables; i++ ) {

            TRUE_OR_RETURN(br->ReadSE(&tmp_sps_qp_table_start_minus26));
            DLOG(INFO) << "## sps_qp_table_start_minus26 : " << tmp_sps_qp_table_start_minus26;
            sps->sps_qp_table_start_minus26.push_back(tmp_sps_qp_table_start_minus26);
            TRUE_OR_RETURN(br->ReadUE(&tmp_sps_num_points_in_qp_table_minus1));
            sps->sps_num_points_in_qp_table_minus1.push_back(tmp_sps_num_points_in_qp_table_minus1);
            DLOG(INFO) << "## sps_num_points_in_qp_table_minus1 : " << tmp_sps_num_points_in_qp_table_minus1;
            
            int tmp_sps_delta_qp_in_val_minus1 = 0;
            int tmp_sps_delta_qp_diff_val = 0;
            //for( int j = 0; j <= sps->sps_num_points_in_qp_table_minus1[ i ]; j++ ) {
            for( size_t j = 0; j <= static_cast<size_t>(sps->sps_num_points_in_qp_table_minus1[ i ]); j++ ) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_delta_qp_in_val_minus1));
              DLOG(INFO) << "## sps_delta_qp_in_val_minus1 : " << tmp_sps_delta_qp_in_val_minus1;
              sps->sps_delta_qp_in_val_minus1[i][j].push_back(tmp_sps_delta_qp_in_val_minus1);
              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_delta_qp_diff_val));
              sps->sps_delta_qp_diff_val[i][j].push_back(tmp_sps_delta_qp_diff_val);
              DLOG(INFO) << "## sps_delta_qp_diff_val : " << tmp_sps_delta_qp_diff_val;
            }
          }
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_sao_enabled_flag));
        DLOG(INFO) << "## sps->sps_sao_enabled_flag : " << ( sps->sps_sao_enabled_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_alf_enabled_flag));
        DLOG(INFO) << "## sps->sps_alf_enabled_flag : " << ( sps->sps_alf_enabled_flag ? "1" : "0");
        if( sps->sps_alf_enabled_flag && sps->sps_chroma_format_idc != 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_ccalf_enabled_flag));
          DLOG(INFO) << "## sps->sps_ccalf_enabled_flag : " << ( sps->sps_ccalf_enabled_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_lmcs_enabled_flag));
        DLOG(INFO) << "## sps->sps_lmcs_enabled_flag : " << ( sps->sps_lmcs_enabled_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_weighted_pred_flag));
        DLOG(INFO) << "## sps->sps_weighted_pred_flag : " << ( sps->sps_weighted_pred_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_weighted_bipred_flag));
        DLOG(INFO) << "## sps->sps_weighted_bipred_flag : " << ( sps->sps_weighted_bipred_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_long_term_ref_pics_flag));
        DLOG(INFO) << "## sps->sps_long_term_ref_pics_flag : " << ( sps->sps_long_term_ref_pics_flag ? "1" : "0");
        if( sps->sps_video_parameter_set_id > 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_inter_layer_prediction_enabled_flag));
          DLOG(INFO) << "## sps->sps_inter_layer_prediction_enabled_flag : " << ( sps->sps_inter_layer_prediction_enabled_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_idr_rpl_present_flag));
        DLOG(INFO) << "## sps->sps_idr_rpl_present_flag : " << ( sps->sps_idr_rpl_present_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_long_term_ref_pics_flag));
        DLOG(INFO) << "## sps->sps_long_term_ref_pics_flag : " << ( sps->sps_long_term_ref_pics_flag ? "1" : "0");
        int tmp_sps_num_ref_pic_lists = 0;
        for( int i = 0; i < ( sps->sps_rpl1_same_as_rpl0_flag ? 1 : 2 ); i++ ) {

          TRUE_OR_RETURN(br->ReadUE(&tmp_sps_num_ref_pic_lists));
          DLOG(INFO) << "## sps_num_ref_pic_lists : " << tmp_sps_num_ref_pic_lists;
          sps->sps_num_ref_pic_lists.push_back(tmp_sps_num_ref_pic_lists);
          for( int j = 0; j < sps->sps_num_ref_pic_lists[ i ]; j++){
            Ref_Pic_List_Struct(i,j, *sps, br, &sps->reference_pic_list_struct);
          }
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_ref_wraparound_enabled_flag));
        DLOG(INFO) << "## sps->sps_ref_wraparound_enabled_flag : " << ( sps->sps_ref_wraparound_enabled_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_temporal_mvp_enabled_flag));
        DLOG(INFO) << "## sps->sps_temporal_mvp_enabled_flag : " << ( sps->sps_temporal_mvp_enabled_flag ? "1" : "0");
        if( sps->sps_temporal_mvp_enabled_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_sbtmvp_enabled_flag));
            DLOG(INFO) << "## sps->sps_sbtmvp_enabled_flag : " << ( sps->sps_sbtmvp_enabled_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_amvr_enabled_flag));
        DLOG(INFO) << "## sps->sps_amvr_enabled_flag : " << ( sps->sps_amvr_enabled_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdof_enabled_flag));
        DLOG(INFO) << "## sps->sps_bdof_enabled_flag : " << ( sps->sps_bdof_enabled_flag ? "1" : "0");
        if(sps->sps_bdof_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdof_control_present_in_ph_flag));
        DLOG(INFO) << "## sps->sps_bdof_control_present_in_ph_flag : " << ( sps->sps_bdof_control_present_in_ph_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_smvd_enabled_flag));
        DLOG(INFO) << "## sps->sps_smvd_enabled_flag : " << ( sps->sps_smvd_enabled_flag ? "1" : "0");
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_dmvr_enabled_flag));
        DLOG(INFO) << "## sps->sps_dmvr_enabled_flag : " << ( sps->sps_dmvr_enabled_flag ? "1" : "0");
        if(sps->sps_dmvr_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_dmvr_control_present_in_ph_flag));
        DLOG(INFO) << "## sps->sps_dmvr_control_present_in_ph_flag : " << ( sps->sps_dmvr_control_present_in_ph_flag ? "1" : "0");
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mmvd_enabled_flag));
        DLOG(INFO) << "## sps->sps_mmvd_enabled_flag : " << ( sps->sps_mmvd_enabled_flag ? "1" : "0");
        if(sps->sps_mmvd_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mmvd_fullpel_only_enabled_flag));
        DLOG(INFO) << "## sps->sps_mmvd_fullpel_only_enabled_flag : " << ( sps->sps_mmvd_fullpel_only_enabled_flag ? "1" : "0");
        }    

        TRUE_OR_RETURN(br->ReadUE(&sps->sps_six_minus_max_num_merge_cand));
        DLOG(INFO) << "## sps->sps_six_minus_max_num_merge_cand : " << sps->sps_six_minus_max_num_merge_cand;

        TRUE_OR_RETURN(br->ReadBool(&sps->sps_sbt_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_affine_enabled_flag));
        if (sps->sps_affine_enabled_flag){
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_five_minus_max_num_subblock_merge_cand));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_6param_affine_enabled_flag));
        }
        if(sps->sps_amvr_enabled_flag){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_affine_amvr_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_affine_prof_enabled_flag));
        if(sps->sps_affine_prof_enabled_flag){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_prof_control_present_in_ph_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_bcw_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_ciip_enabled_flag));
        int MaxNumMergeCand = 6 - sps->sps_six_minus_max_num_merge_cand;
        if (MaxNumMergeCand >= 2){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_gpm_enabled_flag));
          if( sps->sps_gpm_enabled_flag && MaxNumMergeCand >= 3 ){
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_num_merge_cand_minus_max_num_gpm_cand));
          }
        }
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_parallel_merge_level_minus2));

        TRUE_OR_RETURN(br->ReadBool(&sps->sps_isp_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mrl_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mip_enabled_flag));
        if( sps->sps_chroma_format_idc != 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_cclm_enabled_flag));
        }
        if( sps->sps_chroma_format_idc == 1 ) {
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_chroma_horizontal_collocated_flag));
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_chroma_vertical_collocated_flag));      
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_palette_enabled_flag));
        if( sps->sps_chroma_format_idc == 3 && !sps->sps_max_luma_transform_size_64_flag ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_act_enabled_flag));   
        } 
        if( sps->sps_transform_skip_enabled_flag || sps->sps_palette_enabled_flag ){
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_min_qp_prime_ts));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_ibc_enabled_flag));
        if(sps->sps_ibc_enabled_flag){
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_six_minus_max_num_ibc_merge_cand));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_ladf_enabled_flag));
        if(sps->sps_ladf_enabled_flag){
          TRUE_OR_RETURN(br->ReadBits(2,&sps->sps_num_ladf_intervals_minus2));
          TRUE_OR_RETURN(br->ReadSE(&sps->sps_ladf_lowest_interval_qp_offset));
        }
        int tmp_sps_ladf_qp_offset = 0;
        int tmp_sps_ladf_delta_threshold_minus1 = 0;
        for( int i = 0; i < sps->sps_num_ladf_intervals_minus2 + 1; i++ ) {
          TRUE_OR_RETURN(br->ReadSE(&tmp_sps_ladf_qp_offset));
          sps->sps_ladf_qp_offset.push_back(tmp_sps_ladf_qp_offset);
          TRUE_OR_RETURN(br->ReadUE(&tmp_sps_ladf_delta_threshold_minus1));
          sps->sps_ladf_delta_threshold_minus1.push_back(tmp_sps_ladf_delta_threshold_minus1);
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_scaling_list_enabled_flag));
        if( sps->sps_lfnst_enabled_flag && sps->sps_explicit_scaling_list_enabled_flag ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_scaling_matrix_for_lfnst_disabled_flag));
        }
        if( sps->sps_act_enabled_flag && sps->sps_explicit_scaling_list_enabled_flag ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_scaling_matrix_for_alternative_colour_space_disabled_flag));   
        }
        if( sps->sps_scaling_matrix_for_alternative_colour_space_disabled_flag ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_scaling_matrix_designated_colour_space_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_dep_quant_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_sign_data_hiding_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_virtual_boundaries_enabled_flag));
        if(sps->sps_virtual_boundaries_enabled_flag){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_virtual_boundaries_present_flag));
          if(sps->sps_virtual_boundaries_present_flag){
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_num_ver_virtual_boundaries));
            int tmp_sps_virtual_boundary_pos_x_minus1 = 0;
            for( int i = 0; i < sps->sps_num_ver_virtual_boundaries; i++ ){
              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_virtual_boundary_pos_x_minus1));
              sps->sps_virtual_boundary_pos_x_minus1.push_back(tmp_sps_virtual_boundary_pos_x_minus1);
            }
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_num_hor_virtual_boundaries));
            int tmp_sps_virtual_boundary_pos_y_minus1 = 0;
            for( int i = 0; i < sps->sps_num_hor_virtual_boundaries; i++ ){
              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_virtual_boundary_pos_y_minus1));
              sps->sps_virtual_boundary_pos_y_minus1.push_back(tmp_sps_virtual_boundary_pos_y_minus1);
            }
          }
        }
        if( sps->sps_ptl_dpb_hrd_params_present_flag ) {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_timing_hrd_params_present_flag));
          if(sps->sps_timing_hrd_params_present_flag){
            if (!sps->general_timing_hrd_parameters) {
              //sps->general_timing_hrd_parameters = std::make_unique<GeneralTimingHrdParameters>();
              sps->general_timing_hrd_parameters.emplace();
            }
            
            //general_timing_hrd_parameters
            //OK_OR_RETURN(GetGeneralTimingHrdParameters(&sps->general_timing_hrd_parameters,br));
            if (sps->general_timing_hrd_parameters.has_value()) {
               OK_OR_RETURN(GetGeneralTimingHrdParameters(&sps->general_timing_hrd_parameters.value(), br));}
            }
          
          if (sps->max_sublayers_minus1){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_sublayer_cpb_params_present_flag));
          }
          int firstSubLayer = sps->sps_sublayer_cpb_params_present_flag ? 0 : sps->max_sublayers_minus1;


            if (!sps->ols_parameters) {
              //sps->ols_parameters = std::make_unique<H266OlsTimingHrdParameters>();
              sps->ols_parameters.emplace();
            }

 
            if (sps->ols_parameters.has_value()) {
                OK_OR_RETURN(Ols_Timing_Hrd_parameters(firstSubLayer, sps->max_sublayers_minus1,
                                                      *sps,
                                                      br,
                                                      &sps->ols_parameters.value()));
            }
          }
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_field_seq_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_vui_parameters_present_flag));
        if(sps->sps_vui_parameters_present_flag){
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_vui_payload_size_minus1));
          bool sps_vui_alignment_zero_bit;
        
          while(! br->byte_aligned( )){
            TRUE_OR_RETURN(br->ReadBool(&sps_vui_alignment_zero_bit));
          }
         
          OK_OR_RETURN(Vui_Payload(sps->max_sublayers_minus1, br, &sps->vui_parameters));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_extension_flag));
        if(sps->sps_extension_flag){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_range_extension_flag));
          TRUE_OR_RETURN(br->ReadBits(7,&sps->sps_extension_7bits));
          if( sps->sps_range_extension_flag ){
            //todo
            //sps_range_extension( )
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_extended_precision_flag));
            if( sps->sps_transform_skip_enabled_flag ){
                 TRUE_OR_RETURN(br->ReadBool(&sps->sps_ts_residual_coding_rice_present_in_sh_flag));
            }
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_rrc_rice_extension_flag));
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_persistent_rice_adaptation_enabled_flag));
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_reverse_last_sig_coeff_enabled_flag));

          }
        }
        
        if(sps->sps_extension_7bits){
          while( br->more_rbsp_data() ){
            //sps_extension_data_flag
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_extension_data_flag));
        }
      }
      OK_OR_RETURN(rbsp_trailing_bits(br));
      DisplayH266SPS(*sps);
        
      
        
  
  // This will replace any existing SPS instance.
  *sps_id = sps->sps_seq_parameter_set_id;
  active_spses_[*sps_id] = std::move(sps);

  return kOk;
}

#if 1
H266Parser::Result H266Parser::ParseVps(const Nalu& nalu, int* vps_id) {
  DCHECK_EQ(Nalu::H266_VPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 VPS NALU";
  //video_parameter_set_rbsp( )  7.3.2.3  from ITU H266

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *vps_id = -1;
  std::unique_ptr<H266Vps> vps(new H266Vps);

  std::unique_ptr<H266Sps> sps(new H266Sps);


  TRUE_OR_RETURN(br->ReadBits(4, &vps->vps_video_parameter_set_id)); 
  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layers_minus1)); 
  TRUE_OR_RETURN(br->ReadBits(3, &vps->vps_max_sublayers_minus1)); 

  if (vps->vps_max_sublayers_minus1 > 0 && vps->vps_max_sublayers_minus1 >0 ) {
   TRUE_OR_RETURN(br->ReadBool(&vps->vps_default_ptl_dpb_hrd_max_tid_flag));
  }
  if(vps->vps_max_layers_minus1 > 0) {
    //TODO layer_id_included_flag parsing per layer
    TRUE_OR_RETURN(br->ReadBool(&vps->vps_all_independent_layers_flag));
  }
  vps->vps_layer_id.clear();
  int tmp_vps_max_tid_il_ref_pics_plus1 = 0;
  bool tmp_vps_independent_layer_flag = false;
  bool tmp_vps_max_tid_ref_present_flag = false;


  for(uint8_t i=0; i<= vps->vps_max_layers_minus1; i++) {
    int tmp_layer_id;
    TRUE_OR_RETURN(br->ReadBits(6, &tmp_layer_id)); // 6 bits
    uint8_t layerId = static_cast<uint8_t>(tmp_layer_id);
    vps->vps_layer_id[i] = layerId;
    if(i>0 && !vps->vps_all_independent_layers_flag) {
      //TODO parsing of layer_dependency_info( i )
      TRUE_OR_RETURN(br->ReadBool(&tmp_vps_independent_layer_flag));
      vps->vps_independent_layer_flag.push_back(tmp_vps_independent_layer_flag);
      if(!tmp_vps_independent_layer_flag){
        TRUE_OR_RETURN(br->ReadBool(&tmp_vps_max_tid_ref_present_flag));
        vps->vps_max_tid_ref_present_flag.push_back(tmp_vps_max_tid_ref_present_flag);
        for( int j = 0; j < i; j++ ) {
          bool tmp_vps_direct_ref_layer_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_vps_direct_ref_layer_flag));
          vps->vps_direct_ref_layer_flag[i][j] = tmp_vps_direct_ref_layer_flag;

            if( vps->vps_max_tid_ref_present_flag[i] && vps->vps_direct_ref_layer_flag[i][j] ){
                TRUE_OR_RETURN(br->ReadBits(3,&tmp_vps_max_tid_il_ref_pics_plus1));
                vps->vps_max_tid_il_ref_pics_plus1[i][j] = tmp_vps_max_tid_il_ref_pics_plus1;
            }
        }
      }
    }
  }
  bool tmp_vps_each_layer_is_an_ols_flag = false;
  int tmp_vps_ols_mode_idc = 0;
  if( vps->vps_max_layers_minus1 > 0 ) {
    if( vps->vps_all_independent_layers_flag ){
      TRUE_OR_RETURN(br->ReadBool(&tmp_vps_each_layer_is_an_ols_flag));
      vps->vps_each_layer_is_an_ols_flag =tmp_vps_each_layer_is_an_ols_flag;
      if(!tmp_vps_each_layer_is_an_ols_flag){
          TRUE_OR_RETURN(br->ReadBits(3,&tmp_vps_ols_mode_idc));
          vps->vps_ols_mode_idc = tmp_vps_ols_mode_idc;
          if( vps->vps_ols_mode_idc == 2 ) {
            int tmp_vps_num_output_layer_sets_minus2 = 0;
            TRUE_OR_RETURN(br->ReadBits(8,&tmp_vps_num_output_layer_sets_minus2));
            vps->vps_num_output_layer_sets_minus2 = tmp_vps_num_output_layer_sets_minus2;
            bool tmp_vps_ols_output_layer_flag = false;
            for( int i = 1; i <= vps->vps_num_output_layer_sets_minus2 + 1; i ++ ){
              for( u_int32_t j = 0; j <= vps->vps_max_layers_minus1; j++ ){
                TRUE_OR_RETURN(br->ReadBool(&tmp_vps_ols_output_layer_flag));
                vps->vps_ols_output_layer_flag[i][j] = tmp_vps_ols_output_layer_flag;
              }
            }
          }
          int tmp_vps_num_ptls_minus1=0;
          TRUE_OR_RETURN(br->ReadBits(8,&tmp_vps_num_ptls_minus1));
          vps->vps_num_ptls_minus1 = tmp_vps_num_ptls_minus1;
      }
    }
  }
  bool tmp_vps_pt_present_flag;
  int tmp_vps_ptl_max_tid = 0;

  for( int i = 0; i <= vps->vps_num_ptls_minus1; i++ ) {
    if( i > 0 ){
      TRUE_OR_RETURN(br->ReadBool(&tmp_vps_pt_present_flag));
      vps->vps_pt_present_flag.push_back(tmp_vps_pt_present_flag);
    }
    if( !vps->vps_default_ptl_dpb_hrd_max_tid_flag ){
      TRUE_OR_RETURN(br->ReadBits(3,&tmp_vps_ptl_max_tid));
      vps->vps_ptl_max_tid.push_back(tmp_vps_ptl_max_tid);
    }
    
  }
  bool tmp_vps_ptl_alignment_zero_bit = false;
  while(!br->byte_aligned()){
    TRUE_OR_RETURN(br->ReadBool(&tmp_vps_ptl_alignment_zero_bit));
    vps->vps_ptl_alignment_zero_bit = tmp_vps_ptl_alignment_zero_bit;
  }
  for (int i = 0; i <= vps->vps_num_ptls_minus1; i++) {
      if (!vps->vps_ptl.has_value()) {
        vps->vps_ptl.emplace();
      }
      //profile_tier_level( vps_pt_present_flag[ i ], vps_ptl_max_tid[ i ] )
      OK_OR_RETURN(ParseProfileTierLevel(vps->vps_pt_present_flag[i], vps->vps_ptl_max_tid[i], br, 
                                        &vps->vps_ptl.value()));
      
  }
  

  int tmp_vps_ols_ptl_idx = 0;
  int olsModeIdc = 0;

  int TotalNumOlss = 0;
  if( !vps->vps_each_layer_is_an_ols_flag ){
    olsModeIdc = vps->vps_ols_mode_idc;
  } else {
    olsModeIdc = 4;
  }
  if( olsModeIdc == 4 || olsModeIdc == 0 || olsModeIdc == 1 ){
    TotalNumOlss = vps->vps_max_layers_minus1+1;
  } else if( olsModeIdc == 2 ){
    TotalNumOlss = vps->vps_num_output_layer_sets_minus2+2;
  }else{
    LOG(INFO) << "olsModeIdc == 3 ???";
  }
  vps->TotalNumOlss = TotalNumOlss;

  for( int i = 0; i < TotalNumOlss; i++ ){
    if( vps->vps_num_ptls_minus1 > 0 && vps->vps_num_ptls_minus1+1 != TotalNumOlss ){
      TRUE_OR_RETURN(br->ReadBits(8,&tmp_vps_ols_ptl_idx));
      vps->vps_ols_ptl_idx.push_back(tmp_vps_ols_ptl_idx);
    }
  }
  int tmp_vps_num_dpb_params_minus1 = 0;
  if( !vps->vps_each_layer_is_an_ols_flag ) {
    TRUE_OR_RETURN(br->ReadUE(&tmp_vps_num_dpb_params_minus1));
    vps->vps_num_dpb_params_minus1 = tmp_vps_num_dpb_params_minus1;
    bool tmp_vps_sublayer_dpb_params_present_flag = false;
    if( vps->vps_max_sublayers_minus1 > 0 ){
      TRUE_OR_RETURN(br->ReadBool(&tmp_vps_sublayer_dpb_params_present_flag));
      vps->vps_sublayer_dpb_params_present_flag = tmp_vps_sublayer_dpb_params_present_flag;
      int tmp_vps_dpb_max_tid = 0;
      for( int i = 0; i < vps->VpsNumDpbParams; i++ ) {
        if( !vps->vps_default_ptl_dpb_hrd_max_tid_flag ){
          TRUE_OR_RETURN(br->ReadBits(3,&tmp_vps_dpb_max_tid));
          vps->vps_dpb_max_tid.push_back(tmp_vps_dpb_max_tid);
          // TODO
          if(!vps->vps_dpd){
            vps->vps_dpd.emplace();
          }
          dpb_parameters( vps->vps_dpb_max_tid[i],vps->vps_sublayer_dpb_params_present_flag ,
                          &vps->vps_dpd.value(),br);
         
        }
      }
    }
  //}  test comment
  /*########################### Compute external value ############################################################*/

  //The variables NumDirectRefLayers[ i ], DirectRefLayerIdx[ i ][ d ], NumRefLayers[ i ], ReferenceLayerIdx[ i ][ r ], and
  //LayerUsedAsRefLayerFlag[ j ] are derived as follows:
  for( uint32_t i = 0; i <= vps->vps_max_layers_minus1; i++ ) {
    for( uint32_t j = 0; j <= vps->vps_max_layers_minus1; j++ ) {
      vps->dependencyFlag[i][j] = vps->vps_direct_ref_layer_flag[i][j];
      for( uint32_t k = 0; k < static_cast<uint32_t>(i); k++ ){
        if( vps->vps_direct_ref_layer_flag[i][k] && vps->dependencyFlag[k][j] ){
          vps->dependencyFlag[i][j] = true;
        }
      }
      vps->LayerUsedAsRefLayerFlag[i] = false;
    }
  }
  int incd = 0;
  int incr = 0;
  int d = 0;
  int r = 0;
  vps->NumRefLayers.resize(vps->vps_max_layers_minus1 + 1, 0);
  for( uint32_t i = 0; i <= vps->vps_max_layers_minus1; i++ ) {
    for( uint32_t j = 0, d = 0, r = 0; j <= vps->vps_max_layers_minus1; j++ ) {
      incd = d++;
      if( vps->vps_direct_ref_layer_flag[i][j] ) {
        vps->DirectRefLayerIdx[i][incd] = j;
        vps->LayerUsedAsRefLayerFlag[j] = 1;
      }
      incr = r++;
      if( vps->dependencyFlag[i][j] ){
        vps->ReferenceLayerIdx[i][incr] = j;
      }
    }
    vps->NumDirectRefLayers[i].push_back(d);
    vps->NumRefLayers[i] = r;
  }

/*#######################################################################################*/
  //page 100
   vps->NumLayersInOls[0] = 1;
   vps->LayerIdInOls[0][0] = vps->vps_layer_id[0] ;


   vps->NumMultiLayerOlss = 0;
   // Initialize NumOutputLayersInOls and OutputLayerIdInOls for OLS 0
   vps->NumOutputLayersInOls[0] = 1;
   vps->OutputLayerIdInOls[0][0] = vps->vps_layer_id[0];
   vps->NumSubLayersInLayerInOLS[0][0] = vps->vps_ptl_max_tid[vps->vps_ols_ptl_idx[0]] + 1;

   for( int i = 1; i < vps->TotalNumOlss; i++ ) {
    if( vps->vps_each_layer_is_an_ols_flag ) {
      // Each layer is an OLS
      vps->NumLayersInOls[i] = 1;
      vps->LayerIdInOls[i][0] = vps->vps_layer_id[i];

    } else if( vps->vps_ols_mode_idc == 0 || vps->vps_ols_mode_idc == 1 ) {
      // OLS mode 0 or 1
      vps->NumLayersInOls[i] = i+1;
      for( int j = 0; j < vps->NumLayersInOls[i]; j++ ){
        vps->LayerIdInOls[i][j] = vps->vps_layer_id[j];
      }
    } else if( vps->vps_ols_mode_idc == 2 ) {
      // OLS mode 2 
      for( uint32_t k = 0, j = 0; k <= vps->vps_max_layers_minus1; k++ )
      {
         /**************************************************** */
         vps->NumOutputLayersInOls[0] = 1;
         vps->OutputLayerIdInOls[0][0] = vps->vps_layer_id[0];
         vps->NumSubLayersInLayerInOLS[0][0] = vps->vps_ptl_max_tid[vps->vps_ols_ptl_idx[0]]+1;
         // Initialize LayerUsedAsOutputLayerFlag for layers
         for( uint32_t layer_idx = 1; layer_idx <= vps->vps_max_layers_minus1; layer_idx++ ) {
          if( vps->vps_ols_mode_idc == 4 || vps->vps_ols_mode_idc < 2 ){
            vps->LayerUsedAsOutputLayerFlag[layer_idx] = 1;
          }else if( vps->vps_ols_mode_idc == 2 ){
            vps->LayerUsedAsOutputLayerFlag[layer_idx] = 0;
          }
         }
        // Process each OLS for output layers and sublayers
         for( int ols_idx = 1; i < vps->TotalNumOlss; ols_idx++ ){
          if( vps->vps_ols_mode_idc == 4 || vps->vps_ols_mode_idc == 0 ) {
            vps->NumOutputLayersInOls[ols_idx] = 1;
            vps->OutputLayerIdInOls[ols_idx][0] = vps->vps_layer_id[ols_idx];
            if( vps->vps_each_layer_is_an_ols_flag ){
              vps->NumSubLayersInLayerInOLS[ols_idx][0] = vps->vps_ptl_max_tid[vps->vps_ols_ptl_idx[ols_idx]] + 1;
            }else{
              vps->NumSubLayersInLayerInOLS[ols_idx][ols_idx] = vps->vps_ptl_max_tid[vps->vps_ols_ptl_idx[ols_idx]] + 1;
              int maxSublayerNeeded = 0;
              // Process dependencies for lower layers
              for( int k = i -1; k >= 0; k-- ) {
                vps->NumSubLayersInLayerInOLS[ols_idx][k]=0;
                for( int m = k + 1; m <= i; m++ ) {
                  maxSublayerNeeded = std::min(vps->NumSubLayersInLayerInOLS[ols_idx][m],vps->vps_max_tid_il_ref_pics_plus1[m][k]);
                  if( vps->vps_direct_ref_layer_flag[m][k] && vps->NumSubLayersInLayerInOLS[ols_idx][k] < maxSublayerNeeded ){
                    vps->NumSubLayersInLayerInOLS[ols_idx][k] = maxSublayerNeeded;
                  }
                }
              }
            }
          } else if ( vps->vps_ols_mode_idc == 1 ) {
            // OLS mode 1
            vps->NumOutputLayersInOls[i] = i+1;
            for( int j = 0; j < vps->NumOutputLayersInOls[i]; j++ ){
              vps->OutputLayerIdInOls[i][j] = vps->vps_layer_id[j];
              vps->NumSubLayersInLayerInOLS[i][j] = vps->vps_ptl_max_tid[vps->vps_ols_ptl_idx[i]] + 1;
            }
          } else if( vps->vps_ols_mode_idc == 2 ) {
            // OLS mode 2 - complex case
            // Initialize flags and counters
            for( uint32_t j = 0; j <= vps->vps_max_layers_minus1; j++ ) {
              vps->layerIncludedInOlsFlag[i][j] = false;
              vps->NumSubLayersInLayerInOLS[i][j] = 0;
            }
          }
          // Find output layers and set flags
          int highestIncludedLayer = 0;
          for( uint32_t k = 0, j = 0; k <= vps->vps_max_layers_minus1; k++ ){
            if( vps->vps_ols_output_layer_flag[i][k] ) {
              vps->layerIncludedInOlsFlag[i][k] = true;
              highestIncludedLayer = k;
              vps->LayerUsedAsOutputLayerFlag[k] = true;
              vps->OutputLayerIdx[i][j] = k;
              vps->OutputLayerIdInOls[i][j++] = vps->vps_layer_id[k];
              vps->NumSubLayersInLayerInOLS[i][k] = vps->vps_ptl_max_tid[vps->vps_ols_ptl_idx[i]] + 1;
            }
          }
          vps->NumOutputLayersInOls[i] = j;
          int idx = 0;
          // Include reference layers for each output layer
          for( int j = 0; j < vps->NumOutputLayersInOls[ i ]; j++ ) {
            idx = vps->OutputLayerIdx[i][j];
            //NumRefLayers  need to define P97
            // Include all reference layers for this output layer
            for( int k = 0; k < vps->NumRefLayers[idx]; k++ ) {
              //need to populate ReferenceLayerIdx
              int refLayerIdx = vps->ReferenceLayerIdx[idx][k];
              if (!vps->layerIncludedInOlsFlag[i][refLayerIdx] ){
                vps->layerIncludedInOlsFlag[i][refLayerIdx] = 1;
              }
            }
          }
          int maxSublayerNeeded = 0;
          // Calculate sublayer information for included layers
          for( int k = highestIncludedLayer - 1; k >= 0; k-- ){
            if( vps->layerIncludedInOlsFlag[i][k] && !vps->vps_ols_output_layer_flag[i][k] ){
              for( int m = k + 1; m <= highestIncludedLayer; m++ ) {
                maxSublayerNeeded = std::min( vps->NumSubLayersInLayerInOLS[i][m], vps->vps_max_tid_il_ref_pics_plus1[m][k]);
                if( vps->vps_direct_ref_layer_flag[m][k] &&
                   vps->layerIncludedInOlsFlag[i][m] && vps->NumSubLayersInLayerInOLS[i][k] < maxSublayerNeeded ){
                  vps->NumSubLayersInLayerInOLS[i][k] = maxSublayerNeeded;
                }

              }
            }
          }
        }
      }

      /*####################################################################################### */







        //todo p98
        /* if( vps.layerIncludedInOlsFlag[i][k] ){
          vps.LayerIdInOls[i][j++] = vps.vps_layer_id[k];
        } */
        //vps.NumLayersInOls[ i ] = j;
      }
    }
    // if( NumLayersInOls[ i ] > 1 ) {
    //   vps.MultiLayerOlsIdx[ i ] = NumMultiLayerOlss;
    //   vps.NumMultiLayerOlss++;
    // }
  }
  /*################ compute NumMultiLayerOlss #################################*/
  //p 100
    vps->NumMultiLayerOlss = 0;
    int inc_NumLayersInOls = 0;
    uint32_t inc_j = 0;
    uint32_t j = 0;
    for( int i = 1; i < vps->TotalNumOlss; i++ ) {
      if( vps->vps_each_layer_is_an_ols_flag ) {
        vps->NumLayersInOls[i] = 1;
        vps->LayerIdInOls[i][0] = vps->vps_layer_id[i];
      }else if( vps->vps_ols_mode_idc == 0 || vps->vps_ols_mode_idc == 1 ) {
        vps->NumLayersInOls[i] = i + 1;
        inc_NumLayersInOls = vps->NumLayersInOls[i]; 
        for( int j = 0; j < inc_NumLayersInOls; j++ ){
          vps->LayerIdInOls[i][j] = vps->vps_layer_id[j];
        }
      } else if( vps->vps_ols_mode_idc == 2 ) {
        for( uint32_t k = 0, j = 0; k <= vps->vps_max_layers_minus1; k++ ){
          if( vps->layerIncludedInOlsFlag[i][k] ){
            inc_j = j++;
            vps->LayerIdInOls[i][inc_j] = vps->vps_layer_id[k];
          }
        }
        vps->NumLayersInOls[i] = j;
      }
      if( vps->NumLayersInOls[i] > 1 ) {
        vps->MultiLayerOlsIdx[i] = vps->NumMultiLayerOlss;
        vps->NumMultiLayerOlss++;
      }
    }

  /*#################################################*/

  int tmp_vps_ols_dpb_pic_width = 0;
  int tmp_vps_ols_dpb_pic_height = 0;
  int tmp_vps_ols_dpb_chroma_format = 0;
  int tmp_vps_ols_dpb_bitdepth_minus8 =0;
  int tmp_vps_ols_dpb_params_idx = 0;
  for( int i = 0; i < vps->NumMultiLayerOlss; i++ ) {
    TRUE_OR_RETURN(br->ReadUE(&tmp_vps_ols_dpb_pic_width));
    TRUE_OR_RETURN(br->ReadUE(&tmp_vps_ols_dpb_pic_height));
    TRUE_OR_RETURN(br->ReadBits(2,&tmp_vps_ols_dpb_chroma_format));
    TRUE_OR_RETURN(br->ReadUE(&tmp_vps_ols_dpb_bitdepth_minus8));
    vps->vps_ols_dpb_pic_width.push_back(tmp_vps_ols_dpb_pic_width);
    vps->vps_ols_dpb_pic_height.push_back(tmp_vps_ols_dpb_pic_height);
    vps->vps_ols_dpb_chroma_format.push_back(tmp_vps_ols_dpb_chroma_format);
    vps->vps_ols_dpb_bitdepth_minus8.push_back(tmp_vps_ols_dpb_bitdepth_minus8);
  }
  // VpsNumDpbParams p 101
  if( vps->vps_each_layer_is_an_ols_flag ){
    vps->VpsNumDpbParams = 0;
  } else {
    vps->VpsNumDpbParams = vps->vps_num_dpb_params_minus1 + 1;
  }

  if( vps->VpsNumDpbParams > 1 && vps->VpsNumDpbParams != vps->NumMultiLayerOlss ){
        TRUE_OR_RETURN(br->ReadUE(&tmp_vps_ols_dpb_params_idx));
        vps->vps_ols_dpb_params_idx.push_back(tmp_vps_ols_dpb_params_idx);
  }
  bool tmp_vps_timing_hrd_params_present_flag;
  TRUE_OR_RETURN(br->ReadBool(&tmp_vps_timing_hrd_params_present_flag));
  vps->vps_timing_hrd_params_present_flag = tmp_vps_timing_hrd_params_present_flag;
  if(vps->vps_timing_hrd_params_present_flag){
    //general_timing_hrd_parameters( )}
      if (!vps->vps_general_timing_hrd_parameters) {
        vps->vps_general_timing_hrd_parameters.emplace();
      }
      OK_OR_RETURN(GetGeneralTimingHrdParameters(&vps->vps_general_timing_hrd_parameters.value(), br));}

      bool tmp_vps_sublayer_cpb_params_present_flag = false;
      int tmp_vps_num_ols_timing_hrd_params_minus1 = 0;
      if( vps->vps_max_sublayers_minus1 > 0 ){
        TRUE_OR_RETURN(br->ReadBool(&tmp_vps_sublayer_cpb_params_present_flag));
      }
      TRUE_OR_RETURN(br->ReadUE(&tmp_vps_num_ols_timing_hrd_params_minus1));
      vps->vps_num_ols_timing_hrd_params_minus1 = tmp_vps_num_ols_timing_hrd_params_minus1;
      int tmp_vps_hrd_max_tid = 0;
      for( int i = 0; i <= vps->vps_num_ols_timing_hrd_params_minus1; i++ ) {
        if( !vps->vps_default_ptl_dpb_hrd_max_tid_flag ){
          vps->vps_hrd_max_tid.push_back(tmp_vps_hrd_max_tid);
        }
        int firstSubLayer = vps->vps_sublayer_cpb_params_present_flag ? 0 : vps->vps_hrd_max_tid[i];
        //ols_timing_hrd_parameters( firstSubLayer, vps_hrd_max_tid[ i ] );
        if (!vps->vps_ols_parameters) {
              vps->vps_ols_parameters.emplace();
        }

        OK_OR_RETURN(Ols_Timing_Hrd_parameters(firstSubLayer, vps->vps_hrd_max_tid[i],
                                                      *sps,
                                                      br,
                                                      &vps->vps_ols_parameters.value()));
        

      }
      int tmp_vps_ols_timing_hrd_idx = 0;
      if( vps->vps_num_ols_timing_hrd_params_minus1 > 0 && vps->vps_num_ols_timing_hrd_params_minus1+1 != vps->NumMultiLayerOlss ){
        for( int i = 0; i < vps->NumMultiLayerOlss; i++ ){
            TRUE_OR_RETURN(br->ReadUE(&tmp_vps_ols_timing_hrd_idx));
            vps->vps_ols_timing_hrd_idx.push_back(tmp_vps_ols_timing_hrd_idx);
        }
      }
  //}  test comment 
  bool tmp_vps_extension_flag = false;
  bool tmp_vps_extension_data_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_vps_extension_flag));
  
  if(tmp_vps_extension_flag){
    while( br->more_rbsp_data() ){
      TRUE_OR_RETURN(br->ReadBool(&tmp_vps_extension_data_flag));
      vps->vps_extension_data_flag = tmp_vps_extension_data_flag;
    }
  }
  rbsp_trailing_bits(br);
  DisplayH266VPS(pps);
  // This will replace any existing VPS instance.
  *vps_id = vps->vps_video_parameter_set_id;
  active_vpses_[*vps_id] = std::move(vps);

  return kOk;
}
#endif

H266Parser::Result H266Parser::ParseAps(const Nalu& nalu, int* aps_id, int* aps_type) {
  DCHECK(nalu.type() == Nalu::H266_PREFIX_APS_NUT || 
         nalu.type() == Nalu::H266_SUFFIX_APS_NUT);
  LOG(INFO) << "Parsing H.266 APS NALU";

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *aps_id = -1;
  *aps_type = -1;
  std::unique_ptr<H266Aps> aps(new H266Aps);

  TRUE_OR_RETURN(br->ReadUE(aps_type));
  TRUE_OR_RETURN(br->ReadUE(aps_id));

  *aps_id = aps->aps_id;
  *aps_type = aps->aps_type;
  active_apses_[*aps_id] = std::move(aps);

  return kOk;
}

H266Parser::Result H266Parser::ParsePictureHeaderStructure(const Nalu& nalu,
                                                  H266PictureHeaderStructure* phs) {
  DCHECK_EQ(Nalu::H266_PH_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 Picture Header NALU"; 
  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  TRUE_OR_RETURN(br->ReadBool(&phs->ph_gdr_or_irap_pic_flag));
  TRUE_OR_RETURN(br->ReadBool(&phs->ph_non_ref_pic_flag));
  if(phs->ph_gdr_or_irap_pic_flag){
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_gdr_pic_flag));
  }
  TRUE_OR_RETURN(br->ReadBool(&phs->ph_inter_slice_allowed_flag));
  if(phs->ph_inter_slice_allowed_flag){
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_intra_slice_allowed_flag));
  }   
  TRUE_OR_RETURN(br->ReadUE(&phs->ph_pic_parameter_set_id));

  int len_ph_pic_order_cnt_lsb = sps->sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
  TRUE_OR_RETURN(br->ReadBits(len_ph_pic_order_cnt_lsb,&phs->ph_pic_order_cnt_lsb));
  if(phs->ph_gdr_pic_flag){
    TRUE_OR_RETURN(br->ReadUE(&phs->ph_recovery_poc_cnt));
  }
  /************************************************/
  //P 107
  int NumExtraPhBits = 0;
  int max_extra_bytes = sps->sps_num_extra_ph_bytes * 8;
  for( int i = 0; i < max_extra_bytes; i++ ){
    if( sps->sps_extra_ph_bit_present_flag[i]){
      NumExtraPhBits++;
    }
  }
  /************************************************/
  for( int i = 0; i < NumExtraPhBits; i++ ){
    int tmp_ph_extra_bit;
    TRUE_OR_RETURN(br->ReadBool(&tmp_ph_extra_bit));
    phs->ph_extra_bit.push_back(tmp_ph_extra_bit);
  }
  if( sps->sps_poc_msb_cycle_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_poc_msb_cycle_present_flag));
    if(phs->ph_poc_msb_cycle_present_flag){
      int len_ph_poc_msb_cycle_val = sps->sps_poc_msb_cycle_len_minus1 + 1;  
      TRUE_OR_RETURN(br->ReadBits(len_ph_poc_msb_cycle_val,&phs->ph_poc_msb_cycle_val));
    }
  }
  if( sps->sps_alf_enabled_flag && pps->pps_alf_info_in_ph_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_enabled_flag));
    if(phs->ph_alf_enabled_flag){
      TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_num_alf_aps_ids_luma));
      int max_ph_num_alf_aps_ids_luma = phs->ph_num_alf_aps_ids_luma;
      for( int i = 0; i < max_ph_num_alf_aps_ids_luma; i++ ){
      int tmp_ph_alf_aps_id_luma = 0; 
      TRUE_OR_RETURN(br->ReadBits(3,&phs->mp_ph_alf_aps_id_luma));
      }
      if( sps->sps_chroma_format_idc != 0 ) {
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cb_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cr_enabled_flag));
      }

      if( phs->ph_alf_cb_enabled_flag || phs->ph_alf_cr_enabled_flag ){
        TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_alf_aps_id_chroma));
      }
      if (sps->sps_ccalf_enabled_flag ) {
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cc_cb_enabled_flag));
        if(phs->ph_alf_cc_cb_enabled_flag){
          TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_alf_cc_cb_aps_id));
        }
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cc_cr_enabled_flag));
        if(phs->ph_alf_cc_cr_enabled_flag){
          TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_alf_cc_cr_aps_id));
        }
      }
    }  
  }
  if( sps->sps_lmcs_enabled_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_lmcs_enabled_flag));
    if(phs->ph_lmcs_enabled_flag){
      TRUE_OR_RETURN(br->ReadBits(2,&phs->ph_lmcs_aps_id));
      if ( sps->sps_chroma_format_idc != 0 ){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_chroma_residual_scale_flag));
      }
    }  
  }
  if( sps->sps_explicit_scaling_list_enabled_flag ) {
      TRUE_OR_RETURN(br->ReadBool(&ph_explicit_scaling_list_enabled_flag));
      if(phs->ph_explicit_scaling_list_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(3,&phs->ph_scaling_list_aps_id));
      }
  }
  if( sps->sps_virtual_boundaries_enabled_flag && !sps->sps_virtual_boundaries_present_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_virtual_boundaries_present_flag));
    ih(phs->ph_virtual_boundaries_present_flag){
      TRUE_OR_RETURN(br->ReadUE(&phs->ph_num_ver_virtual_boundaries));
      int max_ph_num_ver_virtual_boundaries = phs->ph_num_ver_virtual_boundaries;
      int tmp_ph_virtual_boundary_pos_x_minus1 = 0;
      for( int i = 0; i < max_ph_num_ver_virtual_boundaries; i++ ){
        TRUE_OR_RETURN(br->ReadUE(&tmp_ph_virtual_boundary_pos_x_minus1));
        phs->ph_virtual_boundary_pos_x_minus1.push_back(tmp_ph_virtual_boundary_pos_x_minus1);
      }
      TRUE_OR_RETURN(br->ReadUE(&phs->ph_num_hor_virtual_boundaries));
      int max_ph_num_hor_virtual_boundaries = phs->ph_num_hor_virtual_boundaries;
      int tmp_ph_virtual_boundary_pos_y_minus1 = 0;
      for( int i = 0; i < max_ph_num_hor_virtual_boundaries; i++ ){
        TRUE_OR_RETURN(br->ReadUE(&tmp_ph_virtual_boundary_pos_y_minus1));
        phs->ph_virtual_boundary_pos_y_minus1.push_back(tmp_ph_virtual_boundary_pos_y_minus1);
      }
    }
  }
    if( pps->pps_output_flag_present_flag && !phs->ph_non_ref_pic_flag ){
      TRUE_OR_RETURN(br->ReadBool(&phs->ph_pic_output_flag));
    }
    if( pps->pps_rpl_info_in_ph_flag ){
      phs->rpl.emplace();

      Ref_Pic_List(sps,pps,br,phs->rpl.value());

      //ref_pic_lists( )
    }
    if( sps->sps_partition_constraints_override_enabled_flag ){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_partition_constraints_override_flag));
    }
    if( phs->ph_intra_slice_allowed_flag ) {
        if( phs->ph_partition_constraints_override_flag ) {
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_min_qt_min_cb_intra_slice_luma));
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_max_mtt_hierarchy_depth_intra_slice_luma));
        
          if(phs->ph_max_mtt_hierarchy_depth_intra_slice_luma != 0){
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_bt_min_qt_intra_slice_luma));
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_tt_min_qt_intra_slice_luma));
          }
          if(phs->sps_qtbtt_dual_tree_intra_flag){
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_min_qt_min_cb_intra_slice_chroma);
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_max_mtt_hierarchy_depth_intra_slice_chroma);
            if(phs->ph_max_mtt_hierarchy_depth_intra_slice_chroma != 0 ){
              TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_bt_min_qt_intra_slice_chroma);
              TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_tt_min_qt_intra_slice_chroma);
            }
          }
        }
        if( pps->pps_cu_qp_delta_enabled_flag ){
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_qp_delta_subdiv_intra_slice));
        }
        if( pps->pps_cu_chroma_qp_offset_list_enabled_flag ){
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_chroma_qp_offset_subdiv_intra_slice));
        }        
    }
    if( phs->ph_inter_slice_allowed_flag ) {
      if( phs->ph_partition_constraints_override_flag ) {
        TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_min_qt_min_cb_inter_slice));
        TRUE_OR_RETURN(br->ReadUE(&phs->ph_max_mtt_hierarchy_depth_inter_slice));
        if(phs->ph_max_mtt_hierarchy_depth_inter_slice != 0){
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_bt_min_qt_inter_slice));
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_tt_min_qt_inter_slice));
        }
      }
      if(pps->pps_cu_qp_delta_enabled_flag){
        TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_qp_delta_subdiv_inter_slice));
      }
      if(pps->pps_cu_chroma_qp_offset_list_enabled_flag){
        TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_chroma_qp_offset_subdiv_inter_slice));
      }
      //num_ref_entries
      //RplsIdx

      if(sps->sps_temporal_mvp_enabled_flag){
          TRUE_OR_RETURN(br->ReadBool(&phs->ph_temporal_mvp_enabled_flag));
          if( phs->ph_temporal_mvp_enabled_flag && pps->pps_rpl_info_in_ph_flag ) {

            if( num_ref_entries[ 1 ][ RplsIdx[ 1 ] ] > 0 ){/// evaluate this value
              TRUE_OR_RETURN(br->ReadBool(&phs->ph_collocated_from_l0_flag));
            }
            if( ( phs->ph_collocated_from_l0_flag && num_ref_entries[0][RplsIdx[0] ] > 1 ) || ( !phs->ph_collocated_from_l0_flag && num_ref_entries[ 1 ][ RplsIdx[ 1 ] ] > 1 ) ){
                TRUE_OR_RETURN(br->ReadUE(&phs->ph_collocated_ref_idx));
            }
          }
      }
      if(sps->sps_mmvd_fullpel_only_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_mmvd_fullpel_only_flag));
      }
      bool presenceFlag = false;
      if( !pps->pps_rpl_info_in_ph_flag ){
        presenceFlag = true;
      }
      else ( num_ref_entries[1][ RplsIdx[1] ] > 0 ){
        presenceFlag = true;
      }
      if( presenceFlag ) {
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_mvd_l1_zero_flag));
        if(sps->sps_bdof_control_present_in_ph_flag){
          TRUE_OR_RETURN(br->ReadBool(&phs->ph_bdof_disabled_flag));
        }
        if(sps->sps_dmvr_control_present_in_ph_flag){
          TRUE_OR_RETURN(br->ReadBool(&phs->ph_dmvr_disabled_flag));
        }
      }
      if(sps->sps_prof_control_present_in_ph_flag){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_prof_disabled_flag));
      }
      if( ( pps->pps_weighted_pred_flag || pps->pps_weighted_bipred_flag ) && pps->pps_wp_info_in_ph_flag ){
        //pred_weight_table( )
        phs->p_pwt.emplace();

        PredWeightTable(sps, pps, br, phs->rpl, phs->p_pwt);

      }
    }
    if( pps->pps_qp_delta_info_in_ph_flag ){
      TRUE_OR_RETURN(br->ReadSE(&phs->ph_qp_delta));
    }
    if(sps->sps_joint_cbcr_enabled_flag){
      TRUE_OR_RETURN(br->ReadBool(&phs->ph_joint_cbcr_sign_flag));
    }
    if( sps->sps_sao_enabled_flag && pps->pps_sao_info_in_ph_flag){
      TRUE_OR_RETURN(br->ReadBool(&phs->ph_sao_luma_enabled_flag));
      if(sps->sps_chroma_format_idc != 0){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_sao_chroma_enabled_flag));
      }
    }
    if(pps->pps_dbf_info_in_ph_flag){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_deblocking_params_present_flag));
      if(pps->ph_deblocking_params_present_flag){
        if(!pps->pps_deblocking_filter_disabled_flag){
            TRUE_OR_RETURN(br->ReadBool(&phs->ph_deblocking_filter_disabled_flag));
          if(!phs->ph_deblocking_filter_disabled_flag){
              TRUE_OR_RETURN(br->ReadSE(&));
              TRUE_OR_RETURN(br->ReadSE(&)); 
            if(pps->pps_chroma_tool_offsets_present_flag){
              TRUE_OR_RETURN(br->ReadSE(&phs->ph_cb_beta_offset_div2));
              TRUE_OR_RETURN(br->ReadSE(&phs->ph_cb_tc_offset_div2));
              TRUE_OR_RETURN(br->ReadSE(&phs->ph_cr_beta_offset_div2));
              TRUE_OR_RETURN(br->ReadSE(&phs->ph_cr_tc_offset_div2));
            }
          }
        }
      }
    }
    if( pps->pps_picture_header_extension_present_flag ) {
      TRUE_OR_RETURN(br->ReadUE(&phs->ph_extension_length));
      int max_ph_extension_length = phs->ph_extension_length;
      int tmp_ph_extension_data_byte = 0;
      for( int i = 0; i < max_ph_extension_length; i++){
        int tmp_ph_extension_data_byte = 0;
        TRUE_OR_RETURN(br->ReadUE(&tmp_ph_extension_data_byte));
        phs->ph_extension_data_byte.push_back(tmp_ph_extension_data_byte);
      }
    }

  return kOk;
}




H266Parser::Result H266Parser::ParsePictureHeader(const Nalu& nalu,
                                                  H266PictureHeader* picture_header) {
  DCHECK_EQ(Nalu::H266_PH_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 Picture Header NALU"; 
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
    if (vps->vps_direct_dependency_flag[layer_id][i]) {
      return false;
    }
  }
  return true;
}

const H266Aps* H266Parser::GetAps(int aps_id) {
  return active_apses_[aps_id].get();
}
H266Parser::Result H266Parser::GetGeneralTimingHrdParameters(GeneralTimingHrdParameters *time,
                                      H26xBitReader* br){
  LOG(INFO) << "Parsing H.266 GetGeneralTimingHrdParameters in SPS";
  
  uint32_t num_units, time_scale;
  TRUE_OR_RETURN(br->ReadBits(32, &num_units));
  TRUE_OR_RETURN(br->ReadBits(32, &time_scale));
  time->num_units_in_tick = num_units;
  time->time_scale = time_scale;
  
  TRUE_OR_RETURN(br->ReadBool(&time->general_nal_hrd_params_present_flag));
  TRUE_OR_RETURN(br->ReadBool(&time->general_vcl_hrd_params_present_flag));
  
  if( time->general_nal_hrd_params_present_flag || time->general_vcl_hrd_params_present_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&time->general_same_pic_timing_in_all_ols_flag));
    TRUE_OR_RETURN(br->ReadBool(&time->general_du_hrd_params_present_flag));
    
    if( time->general_du_hrd_params_present_flag ){
      uint32_t tick_divisor;
      TRUE_OR_RETURN(br->ReadBits(8, &tick_divisor));
      time->tick_divisor_minus2 = static_cast<uint8_t>(tick_divisor);
    }
    
    uint32_t bit_rate_scale, cpb_size_scale;
    TRUE_OR_RETURN(br->ReadBits(4, &bit_rate_scale));
    TRUE_OR_RETURN(br->ReadBits(4, &cpb_size_scale));
    time->bit_rate_scale = static_cast<uint8_t>(bit_rate_scale);
    time->cpb_size_scale = static_cast<uint8_t>(cpb_size_scale);
    
    if( time->general_du_hrd_params_present_flag ){
      uint32_t cpb_size_du_scale;
      TRUE_OR_RETURN(br->ReadBits(4, &cpb_size_du_scale));
      time->cpb_size_du_scale = static_cast<uint8_t>(cpb_size_du_scale);
    }
    
    TRUE_OR_RETURN(br->ReadUE(&time->hrd_cpb_cnt_minus1));
  }
  DisplayGeneralTimingHrdParameters(time);
  
  return kOk;
}

#if 0
H266Parser::Result H266Parser::GetGeneralTimingHrdParameters(GeneralTimingHrdParameters *time,
                                      H26xBitReader* br){
  LOG(INFO) << "Parsing H.266 GetGeneralTimingHrdParameters in SPS";
  TRUE_OR_RETURN(br->ReadBits(32,&time->num_units_in_tick));
  TRUE_OR_RETURN(br->ReadBits(32,&time->time_scale));
  TRUE_OR_RETURN(br->ReadBool(&time->general_nal_hrd_params_present_flag));
  TRUE_OR_RETURN(br->ReadBool(&time->general_vcl_hrd_params_present_flag));
  if( time->general_nal_hrd_params_present_flag || time->general_vcl_hrd_params_present_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&time->general_same_pic_timing_in_all_ols_flag));
    TRUE_OR_RETURN(br->ReadBool(&time->general_du_hrd_params_present_flag));
    if( time->general_du_hrd_params_present_flag ){
      uint32_t tick_divisor;
      TRUE_OR_RETURN(br->ReadBits(8, &tick_divisor));
      time->tick_divisor_minus2 = static_cast<uint8_t>(tick_divisor);
    }
    TRUE_OR_RETURN(br->ReadBits(4,&time->bit_rate_scale));
    TRUE_OR_RETURN(br->ReadBits(4,&time->cpb_size_scale));
    if( time->general_du_hrd_params_present_flag ){
      TRUE_OR_RETURN(br->ReadBits(4,&time->cpb_size_du_scale));
    }
    TRUE_OR_RETURN(br->ReadUE(&time->hrd_cpb_cnt_minus1));
  }
    return kOk;
}
#endif
#if 0
// first version light
H266Parser::Result H266Parser::Ref_Pic_List_Struct(int listIdx, int rplsIdx,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266ReferencePicListStruct* rpls){
  LOG(INFO) << "Parsing H.266 Reference picture list structure parameters";
  //7.3.10 Reference picture list structure syntax
  std::vector<std::vector<std::vector<std::vector<int>>>> AbsDeltaPocSt;
  int tmp_num_ref_entries = 0;
  bool tmp_ltrp_in_header_flag = 0;
  bool tmp_inter_layer_ref_pic_flag = 0;
  bool tmp_st_ref_pic_flag = 0;
  //int tmp_abs_delta_poc_st = 0;
  bool tmp_strp_entry_sign_flag = 0;
  int tmp rpls_poc_lsb_lt = 0;
  int tmp_ilrp_idx = 0;
  bool tmp st_ref_pic_flag = 0;

  TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
  //rpls->num_ref_entries[listIdx][rplsIdx].push_back(tmp_num_ref_entries);
  if (rpls->num_ref_entries.size() <= listIdx) {
    rpls->num_ref_entries.resize(listIdx + 1);
  }
  if (rpls->num_ref_entries[listIdx].size() <= rplsIdx) {
    rpls->num_ref_entries[listIdx].resize(rplsIdx + 1);
  }
  rpls->num_ref_entries[listIdx][rplsIdx] = tmp_num_ref_entries;


  
  if( sps.sps_long_term_ref_pics_flag && rplsIdx < sps->sps_num_ref_pic_lists[listIdx] && rpls->num_ref_entries[listIdx][rplsIdx] > 0 ){
    TRUE_OR_RETURN(br->ReadBool(&tmp_ltrp_in_header_flag));
    rpls->ltrp_in_header_flag[ listIdx ][ rplsIdx ].push_back(tmp_ltrp_in_header_flag);
  }
  int num_ref_entries = rpls->num_ref_entries[listIdx][rplsIdx];

  for( int i = 0, j = 0; i <  num_ref_entries; i++) {
    if( sps.sps_inter_layer_prediction_enabled_flag ){
          TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
          rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx][i].push_back(tmp_inter_layer_ref_pic_flag);
    }
    if( !rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx][i] ) {
      if( sps.sps_long_term_ref_pics_flag ){
        TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
        rpls->st_ref_pic_flag[listIdx][rplsIdx][i].push_back(tmp_inter_layer_ref_pic_flag);
      }
      if( rpls->st_ref_pic_flag[listIdx][rplsIdx][i]) {
        TRUE_OR_RETURN(br->ReadUE(&tmp_st_ref_pic_flag));
        rpls->abs_delta_poc_st[listIdx][rplsIdx][i].push_back(tmp_st_ref_pic_flag);
        //compute AbsDeltaPocSt
        int abs_delta_poc_st_value = 0;
        if( ( sps.sps_weighted_pred_flag || sps.sps_weighted_bipred_flag ) && i != 0 )
        {
          AbsDeltaPocSt[listIdx][rplsIdx][i] = rpls->abs_delta_poc_st[listIdx][rplsIdx][i];
        }
        else{
          //AbsDeltaPocSt[listIdx][rplsIdx][i] = rpls->abs_delta_poc_st[listIdx][rplsIdx].at(i) + 1; // [i]+1;
          int abs_delta_poc_st_value = rpls->abs_delta_poc_st[listIdx][rplsIdx].at(i) + 1;
           AbsDeltaPocSt[listIdx][rplsIdx][i] = abs_delta_poc_st_value;

        }
        //if( AbsDeltaPocSt[listIdx][rplsIdx].at(i) > 0 )
        if( abs_delta_poc_st_value > 0 )
        {
          TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
          rpls->strp_entry_sign_flag[ listIdx ][ rplsIdx ][ i ].push_back(tmp_strp_entry_sign_flag);
        }
      
      }     
      
      else if( !rpls->ltrp_in_header_flag[listIdx][rplsIdx] ){
        //The length of the rpls_poc_lsb_lt[ listIdx ][ rplsIdx ][ i ] syntax element is sps_log2_max_pic_order_cnt_lsb_minus4 + 4 bits
        int bit_read = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
        int tmp_rpls_poc_lsb_lt = 0;
        TRUE_OR_RETURN(br->ReadBits(bit_read,&tmp_rpls_poc_lsb_lt));
        rpls->rpls_poc_lsb_lt[ listIdx ][ rplsIdx ][ j++ ].push_back(tmp_rpls_poc_lsb_lt);
      }

    }else{
      TRUE_OR_RETURN(br->ReadUE(&tmp_ilrp_idx));
      rpls->ilrp_idx[listIdx][rplsIdx][i].push_back(tmp_ilrp_idx);
    }
  }
  return kOk;
}
#endif
H266Parser::Result H266Parser::PredWeightTable( const H266Sps& sps, const H266Pps& pps,
                          H26xBitReader* br,
                          H266ReferencePicList *rpl,
                          H266PredWeightTable *pwt){
    LOG(INFO) << "Parsing H.266 Pred Weight Table ";

    // todo add parameter
    // get num_ref_entries  et  RplsIdx  from H266ReferencePicList
    //NumRefIdxActive  equa 139 P157








    TRUE_OR_RETURN(br->ReadUE(&pwt->luma_log2_weight_denom));
    if( sps->sps_chroma_format_idc != 0 ){
      TRUE_OR_RETURN(br->ReadSE(&pwt->delta_chroma_log2_weight_denom));
    }
    if( pps->pps_wp_info_in_ph_flag ){
      TRUE_OR_RETURN(br->ReadUE(&pwt->num_l0_weights));
    }
      /****************************************************************/
    int NumWeightsL0 = 0;
    if( pps->pps_wp_info_in_ph_flag ){
      NumWeightsL0 = rpl->num_l0_weights;
    } else if (!pps->(pps_wp_info_in_ph_flag)){
      NumWeightsL0 = NumRefIdxActive[ 0 ];
    }
    /****************************************************************/


    bool tmp_luma_weight_l0_flag;

    //NumWeightsL0 evalaute 
    int NumWeightsL0 = 0
    for(int i = 0; i < NumWeightsL0; i++ ){
      TRUE_OR_RETURN(br->ReadBool(&tmp_luma_weight_l0_flag));
      pwt->luma_weight_l0_flag.push_back(tmp_luma_weight_l0_flag);
    }
    bool tmp_chroma_weight_l0_flag = false;
    if( sps->sps_chroma_format_idc != 0 ){
      for( int i = 0; i < NumWeightsL0; i++ ){
        TRUE_OR_RETURN(br->ReadBool(&tmp_chroma_weight_l0_flag));
        pwt->chroma_weight_l0_flag.push_back(tmp_chroma_weight_l0_flag);
      }
    }
    int tmp_delta_luma_weight_l0 = 0;
    int tmp_luma_offset_l0 = 0;
    int tmp_delta_chroma_weight_l0 = 0;
    int delta_chroma_offset_l0 = 0
    for( int i = 0; i < NumWeightsL0; i++ ){
      if( rpl->luma_weight_l0_flag[i] ) {
        TRUE_OR_RETURN(br->ReadSE(&tmp_delta_luma_weight_l0));
        TRUE_OR_RETURN(br->ReadSE(&tmp_luma_offset_l0));
        rpl->delta_luma_weight_l0.push_back(tmp_delta_luma_weight_l0);
        rpl->luma_offset_l0.push_back(tmp_luma_offset_l0);
      }
      if( rpl->chroma_weight_l0_flag[i]){
        for(int j = 0; j < 2; j++ ) {
          TRUE_OR_RETURN(br->ReadSE(&tmp_delta_chroma_weight_l0));
          TRUE_OR_RETURN(br->ReadSE(&delta_chroma_offset_l0));
          rpl->delta_chroma_weight_l0.push_back(tmp_delta_chroma_weight_l0);
          rpl->delta_chroma_offset_l0.push_back(delta_chroma_offset_l0);
        }
      }
    }

    int check_entry = num_ref_entries[ 1 ][ RplsIdx[ 1 ] ];
    if( pps->pps_weighted_bipred_flag && pps->pps_wp_info_in_ph_flag && num_ref_entries[ 1 ][ RplsIdx[ 1 ] ] > 0 ){
      TRUE_OR_RETURN(br->ReadSE(&rpl->num_l1_weights));
    }

    int NumWeightsL1 = 0;
    if( !pps->pps_weighted_bipred_flag || ( pps->pps_wp_info_in_ph_flag && num_ref_entries[ 1 ][ RplsIdx[ 1 ] ] == 0 ) ){
      NumWeightsL1 = 0;
    } else if (pps->pps_wp_info_in_ph_flag){
      NumWeightsL1 = rpl->num_l1_weights;
    }
    else{
      NumWeightsL1 = NumRefIdxActive[1];
    }











    bool tmp_luma_weight_l1_flag = false;
    for( int i = 0; i < NumWeightsL1; i++ ){
      TRUE_OR_RETURN(br->ReadBool(&tmp_luma_weight_l1_flag));
      rpl->luma_weight_l1_flag.push_back(tmp_luma_weight_l1_flag);
    }
    bool tmp_chroma_weight_l1_flag = false;
    if( sps->sps_chroma_format_idc != 0 ){
      for( int i = 0; i < NumWeightsL1; i++ ){
        TRUE_OR_RETURN(br->ReadBool(&tmp_chroma_weight_l1_flag));
        pwt->chroma_weight_l1_flag.push_back(tmp_chroma_weight_l1_flag);
      }
    }
    int tmp_delta_luma_weight_l1 = 0;
    int tmp_luma_offset_l1 = 0;
    int tmp_delta_chroma_weight_l1 = 0;
    int delta_chroma_offset_l1 = 0
    for( int i = 0; i < NumWeightsL1; i++ ){
      if( rpl->luma_weight_l1_flag[i] ) {
        TRUE_OR_RETURN(br->ReadSE(&tmp_delta_luma_weight_l1));
        TRUE_OR_RETURN(br->ReadSE(&tmp_luma_offset_l1));
        rpl->delta_luma_weight_l1.push_back(tmp_delta_luma_weight_l1);
        rpl->luma_offset_l1.push_back(tmp_luma_offset_l1);
      }
      if( rpl->chroma_weight_l1_flag[i]){
        for(int j = 0; j < 2; j++ ) {
          TRUE_OR_RETURN(br->ReadSE(&tmp_delta_chroma_weight_l1));
          TRUE_OR_RETURN(br->ReadSE(&delta_chroma_offset_l1));
          rpl->delta_chroma_weight_l1.push_back(tmp_delta_chroma_weight_l1);
          rpl->delta_chroma_offset_l1.push_back(delta_chroma_offset_l1);
        }
      }
return kOk;
}



int ceil_log2(int value) {
    if (value <= 0) return 0; // invalaid value
    if (value == 1) return 0; // log2(1) = 0
    
    return static_cast<int>(std::ceil(std::log2(value)));
}

H266Parser::Result H266Parser::Ref_Pic_List(const H266Sps& sps, const H266Pps& pps,
                            H26xBitReader* br,
                            H266ReferencePicList* rpl) {
    LOG(INFO) << "Parsing H.266 Reference picture list ";
    for( int i = 0; i < 2; i++ ) {
      if( sps->sps_num_ref_pic_lists[i] > 0 && ( i == 0 || ( i == 1 && pps->pps_rpl1_idx_present_flag ) ) ){
        bool tmp_rpl_sps_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_rpl_sps_flag));
        rpl->rpl_sps_flag.push_back(tmp_rpl_sps_flag);
      }
      if(rpl->rpl_sps_flag[i]){
        if( sps->sps_num_ref_pic_lists[i] > 1 && ( i == 0 || ( i == 1 && pps->pps_rpl1_idx_present_flag ) ) ){
          
          int len_rpl_idx = ceil_log2(sps->sps_num_ref_pic_lists[i]);
          int tmp_rpl_idx = 0;
          TRUE_OR_RETURN(br->ReadBits(len_rpl_idx,&tmp_rpl_idx));
          rpl->rpl_idx.push_back(tmp_rpl_idx);     

      } else {

        //ref_pic_list_struct( i, sps_num_ref_pic_lists[ i ] )
        //ref_pic_list_struct( listIdx, rplsIdx )
        rpl->reference_pic_list.emplace();
        Ref_Pic_List_Struct(i, sps->sps_num_ref_pic_lists[i], *sps, br, &rpl->reference_pic_list;);



        /************************************************************ */
        int numLists = 0;
        int numRpls = 0;
        numLists = i;
        numRpls = sps->sps_num_ref_pic_lists[ i ];

        rpl->NumLtrpEntries.resize(i, std::vector<int>(numRpls, 0));

        for(int listIdx = 0; listIdx < numLists; listIdx++) {
        for(int rplsIdx = 0; rplsIdx < numRpls; rplsIdx++) {
            rpl->NumLtrpEntries[listIdx][rplsIdx] = 0;
            
            for(int i = 0; i < rpl->num_ref_entries[listIdx][rplsIdx]; i++) {
                if(!rpl->inter_layer_ref_pic_flag[listIdx][rplsIdx][i] && 
                   !rpl->st_ref_pic_flag[listIdx][rplsIdx][i]) {
                    rpl->NumLtrpEntries[listIdx][rplsIdx]++;
                }
            }
         //init
         rpl->ltrp_in_header_flag[listIdx][rplsIdx] = false;
         if (sps->sps_long_term_ref_pics_flag &&  rplsIdx == sps->sps_num_ref_pic_lists[listIdx]) {
          rpl->ltrp_in_header_flag[listIdx][rplsIdx] = true;
         }
    

        }
        // 8.3.2
        size_t size_sps_num_ref_pic_lists = sps->sps_num_ref_pic_lists.size();
        for (size_t i = 0; i < size_sps_num_ref_pic_lists; i++) {
          rpl->RplsIdx[i] = sps->sps_num_ref_pic_lists[i];
        }
    }
        /************************************************************ */

        bool check_delta_poc_msb_cycle_present_flag = false;
        int tmp_delta_poc_msb_cycle_lt = 0;
        // populate NumLtrpEntries and RplsIdx ltrp_in_header_flag
        for( int j = 0; j < rpl->NumLtrpEntries[i][RplsIdx[i]]; j++ ){
          if( rpl->ltrp_in_header_flag[i][RplsIdx[i] ] ){
            int tmp_poc_lsb_lt = 0;
            int len_poc_lsb_lt = sps->sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
            TRUE_OR_RETURN(br->ReadBits(len_poc_lsb_lt,&tmp_poc_lsb_lt));
            rpl->poc_lsb_lt[i][j] = tmp_poc_lsb_lt;
            check_delta_poc_msb_cycle_present_flag = rpl->delta_poc_msb_cycle_present_flag[i][j]; 
            if(check_delta_poc_msb_cycle_present_flag){
              TRUE_OR_RETURN(br->ReadUE(&tmp_delta_poc_msb_cycle_lt));
              rpl->delta_poc_msb_cycle_lt[i][j] =tmp_delta_poc_msb_cycle_lt;
            }
          }
        }
      }
    }
   
return kOk;
}


H266Parser::Result H266Parser::Ref_Pic_List_Struct(int listIdx, int rplsIdx,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266ReferencePicListStruct* rpls) {
    LOG(INFO) << "Parsing H.266 Reference picture list structure parameters";
    
    int tmp_num_ref_entries = 0;
    bool tmp_ltrp_in_header_flag = false;
    
    TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
    
    rpls->num_ref_entries = tmp_num_ref_entries;
    
    if (sps.sps_long_term_ref_pics_flag && rplsIdx < sps.sps_num_ref_pic_lists[listIdx] && 
        rpls->num_ref_entries > 0) {
        TRUE_OR_RETURN(br->ReadBool(&tmp_ltrp_in_header_flag));
        rpls->ltrp_in_header_flag = tmp_ltrp_in_header_flag;
    }
    
    for (int i = 0; i < rpls->num_ref_entries; i++) {
        H266RefPicListEntry entry;
        bool tmp_inter_layer_ref_pic_flag = false;
        
        if (sps.sps_inter_layer_prediction_enabled_flag) {
            TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
            entry.inter_layer_ref_pic_flag = tmp_inter_layer_ref_pic_flag;
        }
        
        if (!entry.inter_layer_ref_pic_flag) {
            if (sps.sps_long_term_ref_pics_flag) {
                bool tmp_st_ref_pic_flag = false;
                TRUE_OR_RETURN(br->ReadBool(&tmp_st_ref_pic_flag));
                entry.st_ref_pic_flag = tmp_st_ref_pic_flag;
            }
            
            if (entry.st_ref_pic_flag) {
                int tmp_abs_delta_poc_st = 0;
                TRUE_OR_RETURN(br->ReadUE(&tmp_abs_delta_poc_st));
                entry.abs_delta_poc_st = tmp_abs_delta_poc_st;
                
                int abs_delta_poc_st_value = 0;
                if ((sps.sps_weighted_pred_flag || sps.sps_weighted_bipred_flag) && i != 0) {
                    abs_delta_poc_st_value = entry.abs_delta_poc_st;
                } else {
                    abs_delta_poc_st_value = entry.abs_delta_poc_st + 1;
                }
                
                if (abs_delta_poc_st_value > 0) {
                    bool tmp_strp_entry_sign_flag = false;
                    TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
                    entry.strp_entry_sign_flag = tmp_strp_entry_sign_flag;
                }
            } else if (!rpls->ltrp_in_header_flag) {
                uint32_t tmp_rpls_poc_lsb_lt = 0;
                int bit_length = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
                TRUE_OR_RETURN(br->ReadBits(bit_length, &tmp_rpls_poc_lsb_lt));
                entry.rpls_poc_lsb_lt = tmp_rpls_poc_lsb_lt;
            }
        } else {
            int tmp_ilrp_idx = 0;
            TRUE_OR_RETURN(br->ReadUE(&tmp_ilrp_idx));
            entry.ilrp_idx = tmp_ilrp_idx;
        }
        
        rpls->entries.push_back(entry);
    }
    
    return kOk;
}





H266Parser::Result H266Parser::Vui_Payload(int max_num_sub_layers_minus1,
                                                  H26xBitReader* br,
                                                  H266VuiParameters* vui) {
  // Reads whole element but ignores most of it.
  //int ignored;
  LOG(INFO) << "Parsing H.266 VUI parameters";
  //VuiExtensionBitsPresentFlag = 0
  //7.3.2.21 VUI payload syntax  from H266
  // 7.2 VUI parameters syntax from ITU-T H.274 | ISO/IEC 23002-7 */

    TRUE_OR_RETURN(br->ReadBool(&vui->vui_progressive_source_flag));
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_interlaced_source_flag));
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_non_packed_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_non_projected_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_aspect_ratio_info_present_flag));
    if(vui->vui_aspect_ratio_info_present_flag){
      TRUE_OR_RETURN(br->ReadBool(&vui->vui_aspect_ratio_constant_flag));
      TRUE_OR_RETURN(br->ReadBits(8,&vui->vui_aspect_ratio_idc));
      if(vui->vui_aspect_ratio_idc == 255){
        TRUE_OR_RETURN(br->ReadBits(16,&vui->vui_sar_width));
        TRUE_OR_RETURN(br->ReadBits(16,&vui->vui_sar_width));
      }
    }
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_overscan_info_present_flag));
    if(vui->vui_overscan_info_present_flag){
      TRUE_OR_RETURN(br->ReadBool(&vui->vui_overscan_appropriate_flag));
    }
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_colour_description_present_flag));
    if(vui->vui_colour_description_present_flag){
      TRUE_OR_RETURN(br->ReadBits(8,&vui->vui_colour_primaries));
      TRUE_OR_RETURN(br->ReadBits(8,&vui->vui_transfer_characteristics));
      TRUE_OR_RETURN(br->ReadBits(8,&vui->vui_matrix_coeffs));
      TRUE_OR_RETURN(br->ReadBool(&vui->vui_full_range_flag));
    }
    TRUE_OR_RETURN(br->ReadBool(&vui->vui_chroma_loc_info_present_flag));
    if(vui->vui_chroma_loc_info_present_flag){
      if( vui->vui_progressive_source_flag && !vui->vui_interlaced_source_flag ){
        TRUE_OR_RETURN(br->ReadUE(&vui->vui_chroma_sample_loc_type_frame));
      }
      else
      {
        TRUE_OR_RETURN(br->ReadUE(&vui->vui_chroma_sample_loc_type_top_field));
        TRUE_OR_RETURN(br->ReadUE(&vui->vui_chroma_sample_loc_type_bottom_field));
      }
    }
#if 0
  // i wait to remove this code
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
#endif
  return kOk;
}
H266Parser::Result H266Parser::dpb_parameters( int MaxSubLayersMinus1, int subLayerInfoFlag ,
                          H266DPB_Parameters* dpd,
                          H26xBitReader* br){
  LOG(INFO) << "Parsing H.266 dpb_parameters";

 int tmp_dpb_max_dec_pic_buffering_minus1;
 int tmp_dpb_max_num_reorder_pics;
 int tmp_dpb_max_latency_increase_plus1;          
 for(int i  = ( subLayerInfoFlag ? 0 : MaxSubLayersMinus1 ); i <= MaxSubLayersMinus1; i++ ) {
/*     TRUE_OR_RETURN(br->ReadUE(&tmp_dpb_max_dec_pic_buffering_minus1));
    TRUE_OR_RETURN(br->ReadUE(&tmp_dpb_max_num_reorder_pics));
    TRUE_OR_RETURN(br->ReadUE(&tmp_dpb_max_latency_increase_plus1));
 */
    br->ReadUE(&tmp_dpb_max_dec_pic_buffering_minus1);
    br->ReadUE(&tmp_dpb_max_num_reorder_pics);
    br->ReadUE(&tmp_dpb_max_latency_increase_plus1);
    dpd->dpb_max_dec_pic_buffering_minus1.push_back(tmp_dpb_max_latency_increase_plus1);
    dpd->dpb_max_num_reorder_pics.push_back(tmp_dpb_max_num_reorder_pics);
    dpd->dpb_max_latency_increase_plus1.push_back(tmp_dpb_max_latency_increase_plus1);
  }
  return kOk;                  
}

H266Parser::Result H266Parser::Ols_Timing_Hrd_parameters(int firstsublayer, int sps_max_sublayers_minus1,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266OlsTimingHrdParameters* olf){
LOG(INFO) << "Parsing H.266 Ols Timing Hrd parameters";
  //7.3.5.2 OLS timing and HRD parameters 
  bool tmp_fixed_pic_rate_general_flag = false;
  bool tmp_fixed_pic_rate_within_cvs_flag = false;
  int tmp_elemental_duration_in_tc_minus1 = 0;
  bool tmp_low_delay_hrd_flag = false;
  for( int i = firstsublayer; i <= sps_max_sublayers_minus1; i++ ) {

    TRUE_OR_RETURN(br->ReadBool(&tmp_fixed_pic_rate_general_flag));  
    olf->fixed_pic_rate_general_flag.push_back(tmp_fixed_pic_rate_general_flag);
    if( !tmp_fixed_pic_rate_general_flag){
      TRUE_OR_RETURN(br->ReadBool(&tmp_fixed_pic_rate_within_cvs_flag));
      olf->fixed_pic_rate_within_cvs_flag.push_back(tmp_fixed_pic_rate_within_cvs_flag);
      const auto& timing_hrd = sps.general_timing_hrd_parameters.value();

      if(tmp_fixed_pic_rate_within_cvs_flag){
        TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
        olf->elemental_duration_in_tc_minus1.push_back(tmp_elemental_duration_in_tc_minus1);
      //}else if (( sps.general_timing_hrd_parameters.has_value() && sps.general_timing_hrd_parameters.value().general_nal_hrd_params_present_flag || sps.general_timing_hrd_parameters.general_vcl_hrd_params_present_flag ) && sps.general_timing_hrd_parameters.hrd_cpb_cnt_minus1 == 0){
      }else if ( sps.general_timing_hrd_parameters.has_value() && (timing_hrd.general_nal_hrd_params_present_flag || timing_hrd.general_vcl_hrd_params_present_flag) && 
        timing_hrd.hrd_cpb_cnt_minus1 == 0){


        TRUE_OR_RETURN(br->ReadBool(&tmp_low_delay_hrd_flag));
        olf->low_delay_hrd_flag.push_back(tmp_low_delay_hrd_flag);

        //int tmp_bit_rate_value_minus1 = 0;
        int tmp_cpb_size_value_minus1 = 0;
        int tmp_cpb_size_du_value_minus1 = 0;
        int tmp_bit_rate_du_value_minus1 = 0;
        int tmp_cbr_flag = 0;
        const auto& timing_hrd = sps.general_timing_hrd_parameters.value();

        if(timing_hrd.general_nal_hrd_params_present_flag ){
          //todo make function for this 
          for( int j = 0; j <= timing_hrd.hrd_cpb_cnt_minus1; j++ ) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);
            if( timing_hrd.general_du_hrd_params_present_flag ) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_du_value_minus1));
              olf->cpb_size_du_value_minus1[i][j].push_back(tmp_cpb_size_du_value_minus1);
              
              TRUE_OR_RETURN(br->ReadUE(&tmp_bit_rate_du_value_minus1));
              olf->bit_rate_du_value_minus1[i][j].push_back(tmp_bit_rate_du_value_minus1);
            }
            //TRUE_OR_RETURN(br->ReadBool(&tmp_cbr_flag));
            bool cbr_flag;
            TRUE_OR_RETURN(br->ReadBool(&cbr_flag));
            tmp_cbr_flag = cbr_flag ? 1 : 0; 
            olf->cbr_flag[i][j].push_back(tmp_cbr_flag);
          }
        }
        if (sps.general_timing_hrd_parameters.has_value() && 
                      sps.general_timing_hrd_parameters.value().general_vcl_hrd_params_present_flag){
          for( int j = 0; j <= timing_hrd.hrd_cpb_cnt_minus1; j++ ) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);
            if (sps.general_timing_hrd_parameters.has_value() && sps.general_timing_hrd_parameters.value().general_du_hrd_params_present_flag) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_du_value_minus1));
              olf->cpb_size_du_value_minus1[i][j].push_back(tmp_cpb_size_du_value_minus1);
              
              TRUE_OR_RETURN(br->ReadUE(&tmp_bit_rate_du_value_minus1));
              olf->bit_rate_du_value_minus1[i][j].push_back(tmp_bit_rate_du_value_minus1);
            }
            bool cbr_flag_bool;
            TRUE_OR_RETURN(br->ReadBool(&cbr_flag_bool));
            tmp_cbr_flag = cbr_flag_bool ? 1 : 0;
            olf->cbr_flag[i][j].push_back(tmp_cbr_flag);
          }

        }
      }
    }
 return kOk;

 }
return kOk;
}


H266Parser::Result H266Parser::ParseGeneralConstraintsInfo(H266GeneralConstraintsInfo *gci,
                                                     H26xBitReader* br) {
    
TRUE_OR_RETURN(br->ReadBool(&gci->gci_present_flag));
DLOG(INFO) << "gci_present_flag : " << (gci->gci_present_flag ? "1" : "0");
int numAdditionalBitsUsed = 0;
                                                      /* general */
if (gci->gci_present_flag){
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_intra_only_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_all_layers_independent_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_one_au_only_constraint_flag));
    /* picture format */
    TRUE_OR_RETURN(br->ReadBits(4,&gci->gci_sixteen_minus_max_bitdepth_constraint_idc)); //4 bits
    TRUE_OR_RETURN(br->ReadBits(2,&gci->gci_three_minus_max_chroma_format_constraint_idc));//2 bits
    /* NAL unit type related */
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_mixed_nalu_types_in_pic_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_trail_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_stsa_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_rasl_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_radl_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_idr_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_cra_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_gdr_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_aps_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_idr_rpl_constraint_flag));
    /* tile, slice, subpicture partitioning */
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_one_tile_per_pic_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_pic_header_in_slice_header_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_one_slice_per_pic_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_rectangular_slice_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_one_slice_per_subpic_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_subpic_info_constraint_flag));
    /* CTU and block partitioning */
    TRUE_OR_RETURN(br->ReadBits(2,&gci->gci_three_minus_max_log2_ctu_size_constraint_idc)); //2 bits
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_partition_constraints_override_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_mtt_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_qtbtt_dual_tree_intra_constraint_flag));
    /* intra */
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_palette_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ibc_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_isp_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_mrl_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_mip_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_cclm_constraint_flag));
    /* inter */
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ref_pic_resampling_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_res_change_in_clvs_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_weighted_prediction_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ref_wraparound_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_temporal_mvp_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_amvr_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_bdof_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_smvd_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_dmvr_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_mmvd_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_affine_motion_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_prof_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_bcw_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ciip_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_gpm_constraint_flag));
    /* transform, quantization, residual */
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_luma_transform_size_64_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_transform_skip_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_bdpcm_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_mts_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_lfnst_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_joint_cbcr_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_sbt_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_act_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_explicit_scaling_list_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_dep_quant_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_sign_data_hiding_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_cu_qp_delta_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_chroma_qp_offset_constraint_flag));
    /* loop filter */
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_sao_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_alf_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ccalf_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_lmcs_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ladf_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_virtual_boundaries_constraint_flag));
    
      TRUE_OR_RETURN(br->ReadBits(8,&gci->gci_num_additional_bits)); //8 bits
    if(gci->gci_num_additional_bits > 5){
      TRUE_OR_RETURN(br->ReadBool(&gci->gci_all_rap_pictures_constraint_flag));
      TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_extended_precision_processing_constraint_flag));
      TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_ts_residual_coding_rice_constraint_flag));
      TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_rrc_rice_extension_constraint_flag));
      TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_persistent_rice_adaptation_constraint_flag));
      TRUE_OR_RETURN(br->ReadBool(&gci->gci_no_reverse_last_sig_coeff_constraint_flag));
      numAdditionalBitsUsed = 6;
    } else {
      numAdditionalBitsUsed = 0;
    }
    bool tmp_gci_reserved_bit;
    for( int i = 0; i < gci->gci_num_additional_bits-numAdditionalBitsUsed; i++ )
    {
      TRUE_OR_RETURN(br->ReadBool(&tmp_gci_reserved_bit));
      DLOG(INFO) << "gci_reserved_bit : " << (tmp_gci_reserved_bit ? "1" : "0");
    }

  }
  bool gci_alignment_zero_bit = false;  
  while( !br->byte_aligned()){
    TRUE_OR_RETURN(br->ReadBool(&gci_alignment_zero_bit));
    DLOG(INFO) << "gci_alignment_zero_bit : " << (gci_alignment_zero_bit ? "1" : "0");
  }
  return kOk;
}


H266Parser::Result H266Parser::SkipScalingListData(H26xBitReader* br) {
  // H.266 scaling list data parsing would go here
  // Similar to H.265 but with potential differences
  LOG(INFO) << "Skipping H.266 Scaling List Data";
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
/* 
H266Parser::Result H266Parser::ByteAlignment(H26xBitReader* br) {
  LOG(INFO) << "Performing byte alignment";
  TRUE_OR_RETURN(br->SkipBits(1));
  TRUE_OR_RETURN(br->SkipBits(br->NumBitsLeft() % 8));
  return kOk;
}
 */
H266Parser::Result H266Parser::ByteAlignment(H26xBitReader* br) {
    LOG(INFO) << "Performing byte alignment";
    
    // Skip alignment bits
    int bits_to_align = br->NumBitsLeft() % 8;
    if (bits_to_align > 0) {
        TRUE_OR_RETURN(br->SkipBits(bits_to_align));
    }
    
    return kOk;
}

H266Parser::Result H266Parser::rbsp_trailing_bits(H26xBitReader* br) {
  uint32_t stop_bit;
  if (!br->ReadBits(1, &stop_bit)) {
    return kInvalidStream;
  }
  
  if (stop_bit != 1) {
    return kInvalidStream;
  }
  
  // Calculate how many bits until next byte boundary
  int bits_remaining = br->NumBitsLeft() % 8;
  
  // Read rbsp_alignment_zero_bits (must be 0)
  for (int i = 0; i < bits_remaining; i++) {
    uint32_t alignment_bit;
    if (!br->ReadBits(1, &alignment_bit)) {
      return kInvalidStream;
    }
    if (alignment_bit != 0) {
      return kInvalidStream;
    }
  }
  
  return kOk;
}


// Stub implementations for methods that need to be defined
H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu, 
                                               H266SliceHeader* slice_header,
                                               const H266PictureHeader* picture_header) {
  // Implementation would use picture_header context
  LOG(INFO) << "STUB Parsing H.266 Slice Header with Picture Header context";
  //need extract 
/*   sh_slice_type 
  sh_num_ref_idx_active_override_flag 
  sh_num_ref_idx_active_minus1
  to calcultate  NumRefIdxActive[  equation 139 page 157
  Weight Predic  func
 */

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
  LOG(INFO) << "STUB Skipping H.266 ALF Data";
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::SkipLmcsData(H26xBitReader* br) {
  LOG(INFO) << "STUB Skipping H.266 LMCS Data";
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::ParseOlsIds(H26xBitReader* br, std::vector<int>* ols_ids) {
  LOG(INFO) << "STUB Parsing H.266 OLS IDs";
  // Stub implementation
  return kOk;
}

H266Parser::Result H266Parser::ParseDpbParameters(int max_sublayers_minus1,
                                                 bool sublayer_info_flag,
                                                 H26xBitReader* br) {
  // Stub implementation
  LOG(INFO) << "STUB Parsing H.266 DPB Parameters";
  return kOk;
}

H266Parser::Result H266Parser::ParseGeneralConstraintsInfo(H26xBitReader* br) {
  // Stub implementation
  LOG(INFO) << "STUB Parsing H.266 General Constraints Info";
  return kOk;
}
#endif 






#if 0
// First Version 
H266Parser::Result H266Parser::ParseVps(const Nalu& nalu, int* vps_id) {
  DCHECK_EQ(Nalu::H266_VPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 VPS NALU";

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *vps_id = -1;
  std::unique_ptr<H266Vps> vps(new H266Vps);

  // VPS header
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_video_parameter_set_id));
  DLOG(INFO) << "## vps->vps_video_parameter_set_id : " << vps->vps_video_parameter_set_id;
  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layers_minus1));
  DLOG(INFO) << "## vps->vps_max_layers_minus1 : " << vps->vps_max_layers_minus1;
  TRUE_OR_RETURN(br->ReadBits(3, &vps->vps_max_sublayers_minus1));
  DLOG(INFO) << "## vps->vps_max_sublayers_minus1 :  " << vps->vps_max_sublayers_minus1;
  
  // VPS base layer info
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_all_independent_layers_flag));
  DLOG(INFO) << "## vps->vps_all_independent_layers_flag " << (vps->vps_all_independent_layers_flag ? "1" : "0");
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_default_output_layer_idc));
  DLOG(INFO) << "## vps->vps_default_output_layer_idc :" << vps->vps_default_output_layer_idc;

  // Layer IDs
  vps->layer_id_included_flag.resize(vps->vps_max_layers_minus1 + 1, false);
  for(uint32_t i = 1; i <= (vps->vps_max_layers_minus1); i++) {
       bool temp_flag;
       TRUE_OR_RETURN(br->ReadBool(&temp_flag));
       DLOG(INFO) << "## vps->layer_id_included_flag[i] : " << ( temp_flag  ? "1" : "0");
       vps->layer_id_included_flag[i] = temp_flag;
      //TRUE_OR_RETURN(br->ReadBool(&vps->layer_id_included_flag[i]));
  }

  // Timing info
  TRUE_OR_RETURN(br->ReadBool(&vps->vps_timing_info_present_flag));
  DLOG(INFO) << "## vps->vps_timing_info_present_flag : " << ( vps->vps_timing_info_present_flag ? "1" : "0");

  if (vps->vps_timing_info_present_flag) {
    READ_LONG_OR_RETURN(&vps->vps_num_units_in_tick);
    DLOG(INFO) << "## vps->vps_num_units_in_tick :" << vps->vps_num_units_in_tick;
    READ_LONG_OR_RETURN(&vps->vps_time_scale);
    DLOG(INFO) << "## vps->vps_time_scale : " << vps->vps_time_scale;
    
    TRUE_OR_RETURN(br->ReadBool(&vps->vps_poc_proportional_to_timing_flag));
    DLOG(INFO) << "## vps->vps_poc_proportional_to_timing_flag : " << ( vps->vps_poc_proportional_to_timing_flag ? "1" : "0");
    if (vps->vps_poc_proportional_to_timing_flag) {
      
    int temp_int;
    TRUE_OR_RETURN(br->ReadUE(&temp_int));
    vps->vps_num_ticks_poc_diff_one_minus1 = static_cast<uint32_t>(temp_int); 
    DLOG(INFO) << "## vps->vps_num_ticks_poc_diff_one_minus1 : " << temp_int;   
    }
  }

  // Output layer sets
  TRUE_OR_RETURN(br->ReadUE(&vps->vps_num_output_layer_sets));
  DLOG(INFO) << "## vps->vps_num_output_layer_sets : " << vps->vps_num_output_layer_sets;
  
  // Allocate and parse output layer flags
  vps->output_layer_flag.resize(vps->vps_num_output_layer_sets);
  for (uint32_t i = 1; i <= vps->vps_num_output_layer_sets; i++) {
    vps->output_layer_flag[i].resize(vps->vps_max_layers_minus1 + 1, false);
    for (uint32_t j = 0; j <= vps->vps_max_layers_minus1; j++) {

      //TRUE_OR_RETURN(br->ReadBool(&vps->output_layer_flag[i][j]));
      bool temp_output_bool;
      TRUE_OR_RETURN(br->ReadBool(&temp_output_bool));
      vps->output_layer_flag[i][j] = temp_output_bool;
      DLOG(INFO) << "## vps->output_layer_flag[i][j] : " << ( temp_output_bool ? "1" : "0");
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
        DLOG(INFO) << "## vps->direct_dependency_flag[i][j]" << ( temp_dep_bool ? "1" : "0");
        

      }
    }

    for (uint32_t i = 1; i <= vps->vps_max_layers_minus1; i++) {
      //TRUE_OR_RETURN(br->ReadBool(&vps->max_tid_ref_present_flag[i]));
      bool temp_tid_bool;
      TRUE_OR_RETURN(br->ReadBool(&temp_tid_bool));
      vps->max_tid_ref_present_flag[i] = temp_tid_bool;
      DLOG(INFO) << "## vps->max_tid_ref_present_flag[i] :" << ( temp_tid_bool ? "1" : "0");


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
#endif


H266Parser::Result H266Parser::ParseProfileTierLevel(bool profile_tier_present,
                                                     int max_num_sub_layers_minus1,
                                                     H26xBitReader* br,
                                                     H266ProfileTierLevel* ptl) {
    LOG(INFO) << "Parsing H.266 Profile Tier Level";
    
    if (profile_tier_present) {
        uint32_t temp_profile;
        TRUE_OR_RETURN(br->ReadBits(7, &temp_profile));
        ptl->general_profile_idc = static_cast<uint8_t>(temp_profile);

        bool temp_tier;
        TRUE_OR_RETURN(br->ReadBool(&temp_tier));
        ptl->general_tier_flag = temp_tier;
        DLOG(INFO) << "## ptl->general_tier_flag : " << ( ptl->general_tier_flag ? "1" : "0");

    }

    uint32_t temp_level;
    TRUE_OR_RETURN(br->ReadBits(8, &temp_level));
    ptl->general_level_idc = static_cast<uint8_t>(temp_level);
    DLOG(INFO) << "## ptl->general_level_idc : " << temp_level;

    TRUE_OR_RETURN(br->ReadBool(&ptl->ptl_frame_only_constraint_flag));
    DLOG(INFO) << "## ptl->ptl_frame_only_constraint_flag : " << ( ptl->ptl_frame_only_constraint_flag ? "1" : "0");

    TRUE_OR_RETURN(br->ReadBool(&ptl->ptl_multilayer_enabled_flag));
    DLOG(INFO) << "## ptl->ptl_multilayer_enabled_flag : " << ( ptl->ptl_multilayer_enabled_flag ? "1" : "0");

    
    if (profile_tier_present) {
        OK_OR_RETURN(ParseGeneralConstraintsInfo(&ptl->gci, br));
    }
    
    // Parsing des flags de sous-couche
    for (int i = max_num_sub_layers_minus1 - 1; i >= 0; i--) {
        bool tmp_ptl_sublayer_level_present_flag = false;

        TRUE_OR_RETURN(br->ReadBool(&tmp_ptl_sublayer_level_present_flag));
        ptl->ptl_sublayer_level_present_flag.push_back(tmp_ptl_sublayer_level_present_flag);
        DLOG(INFO) << "## ptl->ptl_sublayer_level_present_flag : " << ( ptl->ptl_sublayer_level_present_flag[i] ? "1" : "0");
    }
    while( !br->byte_aligned()){
      bool tmp_ptl_reserved_zero_bit;
      TRUE_OR_RETURN(br->ReadBool( &tmp_ptl_reserved_zero_bit));
      DLOG(INFO) << "## ptl_reserved_zero_bit" << ( tmp_ptl_reserved_zero_bit ? "1" : "0");

    }
    
    // Parsing des niveaux de sous-couche
    for (int i = max_num_sub_layers_minus1 - 1; i >= 0; i--) {
        if (ptl->ptl_sublayer_level_present_flag[i]) {
            uint32_t tmp_sublayer_level_idc;
            TRUE_OR_RETURN(br->ReadBits(8, &tmp_sublayer_level_idc));
            DLOG(INFO) << "## ptl_reserved_zero_bit :" << ( tmp_sublayer_level_idc ? "1" : "0");

            ptl->sublayer_level_idc.push_back(static_cast<uint8_t>(tmp_sublayer_level_idc));
        }
    }
    
    if (profile_tier_present) {
        int tmp_ptl_num_sub_profiles;
        TRUE_OR_RETURN(br->ReadBits(8, &tmp_ptl_num_sub_profiles));
        DLOG(INFO) << "## ptl_num_sub_profiles : " << tmp_ptl_num_sub_profiles;

        ptl->ptl_num_sub_profiles = tmp_ptl_num_sub_profiles ;
        //static_cast<uint8_t>(tmp_ptl_num_sub_profiles);
        //readbits car t read 32 bits need to write another func tu support it
        
        for (int i = 0; i < ptl->ptl_num_sub_profiles; i++) {
            uint32_t tmp_general_sub_profile_idc;
            TRUE_OR_RETURN(br->ReadBits(32, &tmp_general_sub_profile_idc));
            DLOG(INFO) << "## general_sub_profile_idc 32 bits : " << tmp_general_sub_profile_idc;

            ptl->general_sub_profile_idc.push_back(tmp_general_sub_profile_idc);
        }
    }
    
    return kOk;
}

/*
H266Parser::Result H266Parser::ParseProfileTierLevel(bool profile_tier_present,
                                                     int max_num_sub_layers_minus1,
                                                     H26xBitReader* br,
                                                     H266ProfileTierLevel* ptl) {
  LOG(INFO) << "Parsing H.266 Profile Tier Level";
  //7.3.3.1General profile, tier, and level syntax
  bool tmp_ptl_sublayer_level_present_flag = 0;
  int MaxNumSubLayersMinus1 = max_num_sub_layers_minus1;
  //bool ptl_reserved_zero_bit;
  int tmp_sublayer_level_idc;
  u_int32_t tmp_general_sub_profile_idc;

  if (profile_tier_present) {
    // General profile tier level
    //TRUE_OR_RETURN(br->ReadBits(7, &ptl->general_profile_idc));
    //TRUE_OR_RETURN(br->ReadBool(&ptl->general_tier_flag));
    //TRUE_OR_RETURN(br->ReadBits(8, &ptl->general_level_idc));
    uint32_t temp_profile;
    TRUE_OR_RETURN(br->ReadBits(7, &temp_profile));
    ptl->general_profile_idc = static_cast<uint8_t>(temp_profile);

    bool temp_tier;
    TRUE_OR_RETURN(br->ReadBool(&temp_tier));
    ptl->general_tier_flag = temp_tier;
  }

    int temp_level;
    TRUE_OR_RETURN(br->ReadBits(8, &temp_level));
    ptl->general_level_idc = static_cast<uint8_t>(temp_level);

    TRUE_OR_RETURN(br->ReadBool(&ptl->ptl_frame_only_constraint_flag));
    TRUE_OR_RETURN(br->ReadBool(&ptl->ptl_multilayer_enabled_flag));
    if (profile_tier_present) {
      ParseGeneralConstraintsInfo(&ptl->gci,br);
    }
    for( int i = MaxNumSubLayersMinus1-1; i >= 0; i--){
      TRUE_OR_RETURN(br->ReadBool(&tmp_ptl_sublayer_level_present_flag));
      ptl->ptl_sublayer_level_present_flag.push_back(tmp_ptl_sublayer_level_present_flag);
    }
    bool ptl_reserved_zero_bit = false;
     while(!br->byte_aligned()){
      TRUE_OR_RETURN(br->ReadBool(&ptl_reserved_zero_bit));
    } 
    for( int i = MaxNumSubLayersMinus1-1; i >= 0; i-- ){
      if( ptl->ptl_sublayer_level_present_flag[ i ] ){
        TRUE_OR_RETURN(br->ReadBool(&tmp_sublayer_level_idc));
        ptl->sublayer_level_idc.push_back(tmp_sublayer_level_idc);
      }
    }
    if (profile_tier_present) {
      TRUE_OR_RETURN(br->ReadBits(8,&ptl->ptl_num_sub_profiles));
      for( int i = 0; i < ptl->ptl_num_sub_profiles; i++ ){
        TRUE_OR_RETURN(br->ReadBits(32,&tmp_general_sub_profile_idc));
        ptl->general_sub_profile_idc.push_back(tmp_general_sub_profile_idc);

      }
    }

  return kOk;
}
#endif 
*/


bool H266Parser::ParseNalUnits(const uint8_t* data,
                               size_t size,
                               std::vector<NalUnit>* nal_units) {
  LOG(INFO) << "Parsing H.266 NAL units from buffer";

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
