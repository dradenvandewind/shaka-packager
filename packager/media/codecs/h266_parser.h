// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CODECS_H266_PARSER_H_
#define PACKAGER_MEDIA_CODECS_H266_PARSER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <packager/macros/classes.h>
#include <packager/media/codecs/h26x_bit_reader.h>

namespace shaka {
namespace media {

class Nalu;

enum H266SliceType { kBSlice = 0, kPSlice = 1, kISlice = 2 };

const int kMaxRefPicSetCount = 16;

// H.266 profile_tier_level structure is more complex than H.265
const int kGeneralProfileTierLevelBytes = 12;
const int kMaxNumProfileTierLevels = 8;  // Increased for H.266
const int kMaxLayers = 8;  // Increased for H.266 scalability
const int kMaxScalabilityTypes = 8;
const int kMaxLayerIdPlus1 = 64;
const int kMaxLayerSets = 16;
const int kMaxOuputLayerSets = kMaxLayerSets;

const int kInvalidId = -1;

// On success, |coded_width| and |coded_height| contains coded resolution after
// cropping; |pixel_width:pixel_height| contains pixel aspect ratio, 1:1 is
// assigned if it is not present in SPS.
struct H266Sps;
bool ExtractResolutionFromSps(const H266Sps& sps,
                              uint32_t* coded_width,
                              uint32_t* coded_height,
                              uint32_t* pixel_width,
                              uint32_t* pixel_height);

struct H266ReferencePictureSet {
  int delta_poc_s0[kMaxRefPicSetCount];
  int delta_poc_s1[kMaxRefPicSetCount];
  bool used_by_curr_pic_s0[kMaxRefPicSetCount];
  bool used_by_curr_pic_s1[kMaxRefPicSetCount];

  int num_negative_pics;
  int num_positive_pics;
  int num_delta_pocs;
};
struct H266OlsTimingHrdParameters{
    std::vector<bool> fixed_pic_rate_general_flag;
    std::vector<bool> fixed_pic_rate_within_cvs_flag;
    std::vector<int> elemental_duration_in_tc_minus1;
    std::vector<bool> low_delay_hrd_flag;
    std::vector<std::vector<std::vector<int>>> bit_rate_value_minus1;
    std::vector<std::vector<std::vector<int>>> cpb_size_value_minus1;
    std::vector<std::vector<std::vector<int>>> cpb_size_du_value_minus1;
    std::vector<std::vector<std::vector<int>>> bit_rate_du_value_minus1;
    std::vector<std::vector<std::vector<bool>>> cbr_flag;

};

struct H266VuiParameters {
  enum { kExtendedSar = 255 };

  bool aspect_ratio_info_present_flag = false;
  int aspect_ratio_idc = 0;
  int sar_width = 0;
  int sar_height = 0;
  int transfer_characteristics = 0;
  int color_primaries = 0;
  int matrix_coefficients = 0;

  bool vui_timing_info_present_flag = false;
  long vui_num_units_in_tick = 0;
  long vui_time_scale = 0;

  bool bitstream_restriction_flag = false;
  int min_spatial_segmentation_idc = 0;

  // H.266 specific VUI parameters
  bool vui_color_description_present_flag = false;
  bool vui_full_range_flag = false;
  bool vui_chroma_loc_info_present_flag = false;
  int vui_chroma_sample_loc_type_frame = 0;
  int vui_chroma_sample_loc_type_top_field = 0;
  int vui_chroma_sample_loc_type_bottom_field = 0;
  //T-REC-H.274-202309-I!!PDF-E.pdf
  bool vui_progressive_source_flag;
  bool vui_interlaced_source_flag;
  bool vui_non_packed_constraint_flag;
  bool vui_non_projected_constraint_flag;
  bool vui_aspect_ratio_info_present_flag;
  bool vui_aspect_ratio_constant_flag;
  int vui_aspect_ratio_idc;
  int vui_sar_width;
  int vui_sar_height;
  bool vui_overscan_info_present_flag;
  bool vui_overscan_appropriate_flag;

  bool vui_colour_description_present_flag;
  int vui_colour_primaries;
  int vui_transfer_characteristics;
  int vui_matrix_coeffs;
  int vui_full_range_flag;

  bool vui_chroma_loc_info_present_flag;
  u_int vui_chroma_sample_loc_type_frame;
  u_int vui_chroma_sample_loc_type_top_field;
  u_int vui_chroma_sample_loc_type_bottom_field;
  // Incomplete...
};

struct H266Pps {
  H266Pps();
  ~H266Pps();
  
  int pic_parameter_set_id = 0;
  int seq_parameter_set_id = 0;

  // H.266 PPS has different fields than H.265
  bool no_qp_delta_flag = false;
  int init_qp_minus26 = 0;
  bool cu_qp_delta_enabled_flag = false;
  int cu_chroma_qp_offset_list_len_minus1 = 0;

  bool deblocking_filter_override_enabled_flag = false;
  bool deblocking_filter_disabled_flag = false;
  int deblocking_filter_beta_offset_div2 = 0;
  int deblocking_filter_tc_offset_div2 = 0;

  bool rpl_info_in_ph_flag = false;
  bool dbf_info_in_ph_flag = false;
  bool slice_header_extension_present_flag = false;

  bool cross_component_prediction_enabled_flag = false;
  bool chroma_tool_offsets_present_flag = false;
  int log2_sao_offset_scale_luma = 0;
  int log2_sao_offset_scale_chroma = 0;

  // Weighted prediction
  bool weighted_pred_flag = false;
  bool weighted_bipred_flag = false;

  // Tiles and bricks
  bool tiles_enabled_flag = false;
  bool uniform_tile_spacing_flag = true;
  int num_tile_columns_minus1 = 0;
  int num_tile_rows_minus1 = 0;
  std::vector<int> tile_column_width_minus1;
  std::vector<int> tile_row_height_minus1;
  bool loop_filter_across_tiles_enabled_flag = true;

  //pic_parameter_set_rbsp( 7.3.2.5
  int pps_pic_parameter_set_id = 0;
  int pps_seq_parameter_set_id = 0;
  bool pps_mixed_nalu_types_in_pic_flag = false;
  int pps_pic_width_in_luma_samples = 0;
  int pps_pic_height_in_luma_samples = 0;
  
  bool pps_conformance_window_flag = false;
  int pps_conf_win_left_offset = 0;
  int pps_conf_win_right_offset = 0;
  int pps_conf_win_top_offset = 0;
  int pps_conf_win_bottom_offset = 0;

  bool pps_scaling_window_explicit_signalling_flag = false;

  int pps_scaling_win_left_offset = 0;
  int pps_scaling_win_right_offset = 0;
  int pps_scaling_win_top_offset = 0;
  int pps_scaling_win_bottom_offset = 0;

  bool pps_output_flag_present_flag = false;
  bool pps_no_pic_partition_flag = false;
  bool pps_subpic_id_mapping_present_flag = false;

  int pps_num_subpics_minus1 = 0;
  int pps_subpic_id_len_minus1 = 0;
  std::vector<uint32_t> pps_subpic_id;

  int pps_log2_ctu_size_minus5 = 0;
  int pps_num_exp_tile_columns_minus1 = 0;
  int pps_num_exp_tile_rows_minus1 = 0;

  std::vector<uint32_t> pps_tile_column_width_minus1;
  std::vector<uint32_t> pps_tile_row_height_minus1;

  bool pps_loop_filter_across_tiles_enabled_flag = false;
  bool pps_rect_slice_flag = false;
  
  bool pps_single_slice_per_subpic_flag = false;
  int pps_num_slices_in_pic_minus1 = 0;
  bool pps_tile_idx_delta_present_flag = false;
  std::vector<int> pps_slice_width_in_tiles_minus1;
  std::vector<int> pps_slice_height_in_tiles_minus1;
  std::vector<int> pps_num_exp_slices_in_tile;
  //std::vector<int> pps_exp_slice_height_in_ctus_minus1[256]; //256 not sure need check 
  std::vector<std::vector<uint32_t>> pps_exp_slice_height_in_ctus_minus1; 
  std::vector<int> pps_tile_idx_delta_val; //256 not sure need check

  bool pps_loop_filter_across_slices_enabled_flag = false;
  bool pps_cabac_init_present_flag = false;
  std::vector<int> pps_num_ref_idx_default_active_minus1; 


  std::vector<int> pps_num_ref_idx_default_active_minus1; //256 not sure need check
  bool pps_rpl1_idx_present_flag = false;
  bool pps_weighted_pred_flag = false;
  bool pps_weighted_bipred_flag = false;
  bool pps_ref_wraparound_enabled_flag = false;
  int pps_pic_width_minus_wraparound_offset = 0;
  int pps_init_qp_minus26 = 0;
  bool pps_cu_qp_delta_enabled_flag = false;
  
  bool pps_chroma_tool_offsets_present_flag = false;
  int pps_cb_qp_offset = 0;
  int pps_cr_qp_offset = 0;
  bool pps_joint_cbcr_qp_offset_present_flag = false;
  int pps_joint_cbcr_qp_offset_value = 0;

  bool pps_slice_chroma_qp_offsets_present_flag = false;
  bool pps_cu_chroma_qp_offset_list_enabled_flag = false;

  int pps_chroma_qp_offset_list_len_minus1 = 0;
  std::vector<int> pps_qp_offset_list; //256 not sure need check
  std::vector<int> pps_cr_qp_offset_list;
  std::vector<int> pps_joint_cbcr_qp_offset_list;
  std::vector<int> pps_cb_qp_offset_list;
  std::vector<int> pps_cr_qp_offset_list;
  



  bool pps_deblocking_filter_control_present_flag = false;
  bool pps_deblocking_filter_override_enabled_flag = false;
  bool pps_deblocking_filter_disabled_flag = false;
  bool pps_dbf_info_in_ph_flag = false;

  int pps_luma_beta_offset_div2 = 0;
  int pps_luma_tc_offset_div2 = 0;

  int pps_cb_beta_offset_div2 = 0;
  int pps_cb_tc_offset_div2 = 0;
  int pps_cr_beta_offset_div2 = 0;
  int pps_cr_tc_offset_div2 = 0;

  bool pps_rpl_info_in_ph_flag = false;
  bool pps_sao_info_in_ph_flag = false;
  bool pps_alf_info_in_ph_flag = false;
  bool pps_wp_info_in_ph_flag = false;
  bool pps_qp_delta_info_in_ph_flag = false;

  bool pps_picture_header_extension_present_flag = false;
  bool pps_slice_header_extension_present_flag = false;
  bool pps_extension_flag = false;

  bool pps_extension_data_flag = false;
  int CtbSizeY;

};
struct GeneralTimingHrdParameters{
  uint32_t num_units_in_tick;
  uint32_t time_scale;
  bool general_nal_hrd_params_present_flag;
  bool general_vcl_hrd_params_present_flag;
  bool general_same_pic_timing_in_all_ols_flag;
  bool general_du_hrd_params_present_flag;
  uint8_t tick_divisor_minus2;
  int bit_rate_scale;
  int cpb_size_scale;
  int cpb_size_du_scale;
  int hrd_cpb_cnt_minus1;
};


//ref_pic_list_struct( i, j )
struct H266ReferencePicListStruct{
  std::vector<std::vector<std::vector<int>>> num_ref_entries;
  std::vector<std::vector<std::vector<bool>>> ltrp_in_header_flag;
  std::vector<std::vector<std::vector<std::vector<bool>>>> inter_layer_ref_pic_flag;
  std::vector<std::vector<std::vector<std::vector<bool>>>> st_ref_pic_flag;
  std::vector<std::vector<std::vector<std::vector<int>>>> abs_delta_poc_st;
  std::vector<std::vector<std::vector<std::vector<bool>>>> strp_entry_sign_flag;
  std::vector<std::vector<std::vector<std::vector<bool>>>> rpls_poc_lsb_lt;
  std::vector<std::vector<std::vector<std::vector<int>>>> ilrp_idx;
};

struct H266ReferencePicList{
  std::vector <bool> rpl_sps_flag;
  std::vector <int> rpl_idx;
  std::vector<std::vector<std::vector<int>>> poc_lsb_lt;
  std::vector<std::vector<std::vector<bool>>> delta_poc_msb_cycle_present_flag;
  std::vector<std::vector<std::vector<int>>> delta_poc_msb_cycle_lt;
  H266ReferencePicListStruct reference_pic_list;
};




struct H266Sps {
  H266Sps();
  ~H266Sps();

  int GetPicSizeInCtbsY() const;
  int GetChromaArrayType() const;

  uint32_t GetBitDepthLuma() const;
  uint32_t GetBitDepthChroma() const;
  uint32_t GetQpBdOffset() const;
  bool IsValidBitDepth() const;
 

  int sps_seq_parameter_set_id = 0; // 4 bits
  int vps_id = 0;  // H.266 uses vps_id directly in SPS
  int sps_video_parameter_set_id = 0;  // 4 bits
  int max_sublayers_minus1 = 0;// 3 bits
  int sps_chroma_format_idc = 1; // default to 4:2:0
  int sps_log2_ctu_size_minus5 = 0; // default to 0 (32x32 CTU) 2 bits
  bool sps_ptl_dpb_hrd_params_present_flag = false;
  bool sps_gdr_enabled_flag = false;
  bool sps_ref_pic_resampling_enabled_flag = false;
  bool sps_res_change_in_clvs_allowed_flag = false;
  int sps_pic_width_in_luma_samples = 0;
  int sps_pic_width_max_in_luma_samples = 0;

  bool sps_conformance_window_flag = false;
  int sps_conf_win_left_offset = 0;
  int sps_conf_win_right_offset = 0;
  int sps_conf_win_top_offset = 0;
  int sps_conf_win_bottom_offset = 0;

  bool sps_subpic_info_present_flag = false;
  int sps_num_subpics_minus1 = 0;
  bool sps_independent_subpics_flag = false;
  std::vector <int> sps_subpic_ctu_top_left_x;
  std::vector <int> sps_subpic_ctu_top_left_y;
  std::vector <int> sps_subpic_width_minus1;
  std::vector <int> sps_subpic_height_minus1;
  

  bool sps_subpic_same_size_flag = false;
  std::vector<int> sps_subpic_top_left_x;
  std::vector<int> sps_subpic_top_left_y;
  std::vector<int> sps_subpic_width_minus1;
  std::vector<int> sps_subpic_height_minus1;
  std::vector<bool> sps_subpic_treated_as_pic_flag;
  std::vector<bool> sps_loop_filter_across_subpic_enabled_flag;
  bool sps_subpic_id_len_minus1 = false;
  bool sps_subpic_id_mapping_explicitly_signalled_flag = false;
  
  bool sps_subpic_id_mapping_present_flag = false;
  std::vector<uint32_t> sps_subpic_id;
  int sps_bitdepth_minus8 = 0;
  bool sps_entropy_coding_sync_enabled_flag = false;
  bool sps_entry_point_offsets_present_flag = false;
  int sps_log2_max_pic_order_cnt_lsb_minus4 = 0;
  bool sps_poc_msb_cycle_flag = false;
  int sps_poc_msb_cycle_len_minus1 = 0;
  int sps_num_extra_ph_bytes = 0;


  std::vector<bool> sps_extra_ph_bit_present_flag;//256 not sure need check
  
  
  int sps_num_extra_sh_bytes = 0;
  std::vector<bool> sps_extra_sh_bit_present_flag;//256 not sure need check
  bool sps_sublayer_dpb_params_flag = false;

  int sps_log2_min_luma_coding_block_size_minus2 = 0;
  bool sps_partition_constraints_override_enabled_flag = false;
  int sps_log2_diff_min_qt_min_cb_intra_slice_luma = 0;
  int sps_max_mtt_hierarchy_depth_intra_slice_luma = 0;
  int sps_log2_diff_max_bt_min_qt_intra_slice_luma = 0;
  int sps_log2_diff_max_tt_min_qt_intra_slice_luma = 0;

  bool sps_qtbtt_dual_tree_intra_flag = false;
  int sps_log2_diff_min_qt_min_cb_intra_slice_chroma = 0;
  int sps_log2_diff_max_tt_min_qt_intra_slice_chroma = 0;

  int sps_log2_diff_min_qt_min_cb_inter_slice = 0;
  int sps_max_mtt_hierarchy_depth_inter_slice = 0;

  int sps_log2_diff_max_bt_min_qt_inter_slice = 0;
  int sps_log2_diff_max_tt_min_qt_inter_slice = 0;

  bool sps_max_luma_transform_size_64_flag = false;
  bool sps_transform_skip_enabled_flag = false;

  int sps_log2_transform_skip_max_size_minus2 = 0;
  int sps_bdpcm_enabled_flag = 0;
  
  bool sps_mts_enabled_flag = false;
  bool sps_explicit_mts_intra_enabled_flag = false;
  bool sps_explicit_mts_inter_enabled_flag = false;
  bool sps_lfnst_enabled_flag = false;
  bool sps_joint_cbcr_enabled_flag = false;
  bool sps_same_qp_table_for_chroma_flag = false;

  std::vector <u_int> sps_qp_table_start_minus26;//not sure need check
  std::vector <u_int> sps_num_points_in_qp_table_minus1;//not sure need check
  
  std::vector<std::vector<std::vector<int>>> sps_delta_qp_in_val_minus1;//not sure need check
  std::vector<std::vector<std::vector<int>>> sps_delta_qp_diff_val;//not sure need check

  bool sps_sao_enabled_flag = false;
  bool sps_ccalf_enabled_flag = false;
  bool sps_lmcs_enabled_flag = false;
  bool sps_weighted_pred_flag = false;
  bool sps_weighted_bipred_flag = false;
  bool sps_long_term_ref_pics_flag = false;
  H266ReferencePic pic;
  bool sps_inter_layer_prediction_enabled_flag = false;
  bool sps_idr_rpl_present_flag = false;
  bool sps_rpl1_same_as_rpl0_flag = false;
  std::vector <int> sps_num_ref_pic_lists;
  H266ReferencePicListStruct reference_pic_list_struct;



  bool sps_ref_wraparound_enabled_flag = false;
  bool sps_temporal_mvp_enabled_flag = false;
  bool sps_sbtmvp_enabled_flag = false;
  bool sps_amvr_enabled_flag = false;
  bool sps_bdof_enabled_flag = false;
  bool sps_bdof_control_present_in_ph_flag = false;
  bool sps_smvd_enabled_flag = false;
  bool sps_dmvr_enabled_flag = false;

  bool sps_dmvr_control_present_in_ph_flag = false;
  bool sps_mmvd_enabled_flag = false;

  bool sps_mmvd_fullpel_only_enabled_flag = false;
  int sps_six_minus_max_num_merge_cand = false;
  bool sps_sbt_enabled_flag = false;
  bool sps_affine_enabled_flag = false;

  int sps_five_minus_max_num_subblock_merge_cand = 0;
  bool sps_6param_affine_enabled_flag = false;

  bool sps_affine_amvr_enabled_flag = false;
  bool sps_affine_prof_enabled_flag = false;

  bool sps_prof_control_present_in_ph_flag = false;
  bool sps_bcw_enabled_flag = false;
  
  bool sps_ciip_enabled_flag = false;
  bool sps_gpm_enabled_flag = false;

  int sps_max_num_merge_cand_minus_max_num_gpm_cand = 0;
  int sps_log2_parallel_merge_level_minus2 = 0;
  bool sps_isp_enabled_flag = false;
  bool sps_mrl_enabled_flag = false;
  bool sps_mip_enabled_flag = false;

  bool sps_cclm_enabled_flag = false;

  bool sps_chroma_horizontal_collocated_flag = false;
  bool sps_chroma_vertical_collocated_flag = false;

  bool sps_palette_enabled_flag = false;
  bool sps_act_enabled_flag = false;

  int sps_min_qp_prime_ts = 0;
  bool sps_ibc_enabled_flag = false;

  int sps_six_minus_max_num_ibc_merge_cand = 0;

  bool sps_ladf_enabled_flag = false;

  int sps_num_ladf_intervals_minus2 = 0;
  int sps_ladf_lowest_interval_qp_offset = 0;

  std::vector<int> sps_ladf_qp_offset; //3 not sure need check sps_num_ladf_intervals_minus2
  std::vector<int> sps_ladf_delta_threshold_minus1;//3 not sure need check

  bool sps_explicit_scaling_list_enabled_flag = false;
  bool sps_scaling_matrix_for_lfnst_disabled_flag = false;

  bool sps_scaling_matrix_for_alternative_colour_space_disabled_flag = false;
  bool sps_scaling_matrix_designated_colour_space_flag = false;
  bool sps_dep_quant_enabled_flag = false;
  bool sps_sign_data_hiding_enabled_flag = false;

  bool sps_virtual_boundaries_enabled_flag = false;
  bool sps_virtual_boundaries_present_flag = false;

  int sps_num_ver_virtual_boundaries = 0;

  std::vector <int> sps_virtual_boundary_pos_x_minus1[12];//i = sps_num_ver_virtual_boundaries
  int sps_num_hor_virtual_boundaries = 0;

  std::vector <int> sps_virtual_boundary_pos_y_minus1;//i = sps_num_hor_virtual_boundaries  

  bool sps_timing_hrd_params_present_flag = false;
  GeneralTimingHrdParameters timing;
  bool sps_sublayer_cpb_params_present_flag = false;

  bool sps_vui_parameters_present_flag = false;
  bool sps_sublayer_cpb_params_present_flag = false;

  bool sps_field_seq_flag = false;
  bool sps_vui_parameters_present_flag = false;

  int sps_vui_payload_size_minus1 = 0;  

  bool sps_vui_alignment_zero_bits = false;
  bool sps_extension_flag = false;

  bool sps_range_extension_flag = false;
  bool sps_extension_7bits_flag = false;

  bool sps_extension_data_flag = false;

 // end H.266 specific fields


  bool sps_temporal_id_nesting_flag = false;

  // H.266 profile_tier_level structure
  int general_profile_tier_level_data[12] = {};

  int chroma_format_idc = 0;
  int pic_width_max_in_luma_samples = 0;
  int pic_height_max_in_luma_samples = 0;

  // Conformance window
  bool conformance_window_present_flag = false;
  int conf_win_left_offset = 0;
  int conf_win_right_offset = 0;
  int conf_win_top_offset = 0;
  int conf_win_bottom_offset = 0;

  // Bit depth
  int bit_depth_luma_minus8 = 0;
  int bit_depth_chroma_minus8 = 0;

  // Partitioning
  int log2_ctu_size_minus5 = 0;
  int log2_min_luma_coding_block_size_minus2 = 0;

  // Quantization
  int qp_bd_offset = 0;

  // Temporal MVP
  bool sps_temporal_mvp_enabled_flag = false;

  // Strong intra smoothing
  bool sps_strong_intra_smoothing_enabled_flag = false;
  // OLS timing hrd parameters
  H266OlsTimingHrdParameters ols_parameters;

  // VUI parameters
  bool vui_parameters_present = false;
  H266VuiParameters vui_parameters;


  // H.266 specific tools
  bool sps_affine_enabled_flag = false;
  bool sps_amvr_enabled_flag = false;
  bool sps_bdof_enabled_flag = false;
  bool sps_bdof_control_present_in_ph_flag = false;
  bool sps_sao_enabled_flag = false;
  bool sps_alf_enabled_flag = false;

  // Sub-picture and scalability
  bool sps_subpic_treated_as_pic_flag = false;
  bool sps_ref_wraparound_enabled_flag = false;

  // Incomplete: many more H.266 specific fields...
};

struct H266RepFormat {
  int pic_width_vps_in_luma_samples = 0;
  int pic_height_vps_in_luma_samples = 0;

  int chroma_format_vps_idc = 0;
  bool separate_colour_plane_vps_flag = false;

  int bit_depth_vps_luma_minus8 = 0;
  int bit_depth_vps_chroma_minus8 = 0;

  int conf_win_vps_left_offset = 0;
  int conf_win_vps_right_offset = 0;
  int conf_win_vps_top_offset = 0;
  int conf_win_vps_bottom_offset = 0;
};
bool sps_subpic_info_present_flag =

struct H266ProfileTierLevel {
  uint8_t general_profile_idc;
  uint8_t general_tier_flag;
  uint8_t general_level_idc;
  bool general_frame_only_constraint_flag;
  bool general_non_packed_constraint_flag;
  bool general_interlaced_source_flag;
  bool general_progressive_source_flag;
  // ... autres champs du profile tier level
};


struct H266Vps {
  H266Vps();
  ~H266Vps();

  enum {
    kTexture = 0,
    kMultiview = 1,
    kSpatial = 2,
    kAuxiliary = 3,
    kNone = 16
  };

  int vps_video_parameter_set_id; //4 bits
  //int vps_max_layers_minus1;
  uint32_t vps_max_layers_minus1; //6 bits
  int vps_max_sublayers_minus1; //3 bits
  bool vps_default_ptl_dpb_hrd_max_tid_flag; //bool
  bool vps_all_independent_layers_flag; //bool

  std::vector<uint8_t> vpsLayerId; // 6 bits each, size = vpsMaxLayersMinus1 + 1
  std::vector<bool> vps_independent_layer_flag;

  // Timing info in VPS (H.266 specific)
  bool vps_timing_info_present_flag;
  long vps_num_units_in_tick;
  long vps_time_scale;

  // General constraints
  bool vps_each_layer_is_an_ols_flag;
  int vps_ols_mode_idc;

  // Output layer sets
  int vps_num_output_layer_sets_minus1;
  int vps_num_ptls_minus1;

  // Profile tier level
  int general_profile_tier_level_data[kMaxNumProfileTierLevels]
                                     [kGeneralProfileTierLevelBytes];

  // Layer sets
  int vps_num_layer_sets_minus1;
  int vps_max_layer_id;

  // Scalability info
  int scalability_type = kNone;

  // H.266 specific: OPI (Operating Point Information) support
  bool vps_opi_present_flag;

  /*                */
  bool vps_default_output_layer_idc;
  //bool vps_all_independent_layers_flag;
  //std::vector<uint32_t> layer_id_included_flag;
  std::vector<bool> layer_id_included_flag;


  // Timing info
  bool vps_poc_proportional_to_timing_flag;
  uint32_t vps_num_ticks_poc_diff_one_minus1;
  
  // Output layer sets
  uint32_t vps_num_output_layer_sets;
  std::vector<std::vector<bool>> output_layer_flag;
  
  // Profile tier level
  H266ProfileTierLevel profile_tier_level;
  
  // Layer dependency
  std::vector<std::vector<bool>> direct_dependency_flag;
  std::vector<uint32_t> max_tid_ref_present_flag;
  

  // Incomplete: many more H.266 VPS specific fields...
};

struct H266Aps {
  H266Aps();
  ~H266Aps();

  int aps_id = 0;
  int aps_type = 0;  // ALF, LMCS, SCALING_LIST

  // Adaptation parameter set type specific data would go here
  // This is a simplified version
};

struct H266PictureHeader {
  H266PictureHeader();
  ~H266PictureHeader();

  bool ph_gdr_or_irap_pic_flag = false;
  bool ph_non_ref_pic_flag = false;
  int ph_pic_parameter_set_id = 0;
  int ph_pic_order_cnt_lsb = 0;
  
  // Reference picture lists
  bool ph_rpl_present_flag = false;
  int num_ref_idx_active_override_flag = 0;
  
  // Deblocking filter
  bool ph_deblocking_filter_override_flag = false;
  bool ph_deblocking_filter_disabled_flag = false;
  int ph_beta_offset_div2 = 0;
  int ph_tc_offset_div2 = 0;
  
  // Quantization
  int ph_qp_delta = 0;
  
  // Weighted prediction
  bool ph_weighted_pred_flag = false;
  bool ph_weighted_bipred_flag = false;
  
  // Temporal MVP
  bool ph_temporal_mvp_enabled_flag = false;
};

struct H266SliceHeader {
  H266SliceHeader();
  ~H266SliceHeader();

  // Many of the fields here are required when parsing so the default here may
  // not be valid.

  size_t header_bit_size = 0;

  int pic_parameter_set_id = 0;
  int slice_type = 0;
  bool no_output_of_prior_pics_flag = false;
  
  // Picture order count
  int pic_order_cnt_lsb = 0;
  
  // Reference picture lists
  bool slice_rpl_present_flag = false;
  int num_ref_idx_l0_active_minus1 = 0;
  int num_ref_idx_l1_active_minus1 = 0;
  
  // Prediction weights
  bool slice_pred_weights_flag = false;
  
  // Quantization
  int slice_qp_delta = 0;
  int slice_cb_qp_offset = 0;
  int slice_cr_qp_offset = 0;
  
  // Deblocking filter
  bool slice_deblocking_filter_override_flag = false;
  bool slice_deblocking_filter_disabled_flag = false;
  int slice_beta_offset_div2 = 0;
  int slice_tc_offset_div2 = 0;
  
  // ALF
  bool slice_alf_enabled_flag = false;
  
  // BDOF and DMVR
  bool slice_bdof_flag = false;
  bool slice_dmvr_flag = false;
  
  // First slice segment flag
  bool first_slice_segment_in_pic_flag = false;
  
  // Dependent slice segment
  bool dependent_slice_segment_flag = false;
  int slice_segment_address = 0;
  // slice_header 7.3.7
  bool sh_picture_header_in_slice_header_flag = false;
  int sh_subpic_id = 0;
  int sh_slice_address = 0;
  std::vector<int> sh_extra_bits; //256 not sure need check
  int sh_num_tiles_in_slice_minus1 = 0;
  int sh_slice_type = 0;
  bool sh_no_output_of_prior_pics_flag = false;
  bool sh_alf_enabled_flag = false;
  int sh_num_alf_aps_ids_luma = 0;
  std::vector<int> sh_alf_aps_id_luma; //not sure need check

  bool sh_alf_cb_enabled_flag = false;
  bool sh_alf_cr_enabled_flag = false;
  int sh_alf_aps_id_chroma = 0;
  bool sh_alf_cc_cb_enabled_flag = false;
  int sh_alf_cc_cb_aps_id = 0;
  bool sh_alf_cc_cr_enabled_flag = false;
  int sh_alf_cc_cr_aps_id = 0;

  bool sh_lmcs_used_flag = false;
  bool sh_explicit_scaling_list_used_flag = false;
  bool sh_num_ref_idx_active_override_flag = false;

  std::vector<int> sh_num_ref_idx_active_minus1; 
  bool sh_cabac_init_flag = false;
  bool sh_collocated_from_l0_flag = false;
  int sh_collocated_ref_idx = 0;

  int sh_qp_delta = 0;
  int sh_cb_qp_offset = 0;
  int sh_cr_qp_offset = 0;
  int sh_joint_cbcr_qp_offset = 0;
  bool sh_cu_chroma_qp_offset_enabled_flag = false;
  bool sh_sao_luma_used_flag = false; 

  bool sh_sao_chroma_used_flag = false;
  bool sh_deblocking_params_present_flag = false;
  bool sh_deblocking_filter_disabled_flag = false;
  int sh_beta_offset_div2 = 0;
  int sh_tc_offset_div2 = 0;

  int sh_cb_beta_offset_div2 = 0;
  int sh_cb_tc_offset_div2 = 0;
  int sh_cr_beta_offset_div2 = 0;
  int sh_cr_tc_offset_div2 = 0;

  bool sh_dep_quant_used_flag = false;
  bool sh_sign_data_hiding_used_flag = false;
  bool sh_ts_residual_coding_disabled_flag = false;

  int sh_ts_residual_coding_rice_idx_minus1 = 0;
  int sh_reverse_last_sig_coeff_flag = 0;
  int sh_slice_header_extension_length  = 0;
  std::vector<bool> sh_slice_header_extension_data_byte; //not sure need check

int sh_entry_offset_len_minus1 = 0;
std::vector<uint32_t> sh_entry_point_offset_minus1; //256 not sure need check

};
 
  
/// A class to parse H.266 streams.
class H266Parser {
 public:
   struct NalUnit {
    const uint8_t* data;
    size_t size;
    int type;
  };
  enum Result {
    kOk,
    kInvalidStream,      // error in stream
    kUnsupportedStream,  // stream not supported by the parser
    kEOStream,           // end of stream
  };

  H266Parser();
  ~H266Parser();



  bool GetVpsTimingInfo(int vps_id, uint32_t* num_units_in_tick, uint32_t* time_scale);
  uint32_t GetMaxLayers(int vps_id);
  bool IsLayerIndependent(int vps_id, uint32_t layer_id);

  /// Parses a video slice header.
  Result ParseSliceHeader(const Nalu& nalu, H266SliceHeader* slice_header);
  Result ParseSliceHeader(const Nalu& nalu,
                          H266SliceHeader* slice_header,
                          const H266PictureHeader* picture_header);

  /// Parses a slice header with picture header context
  /* 
  Result ParseSliceHeader(const Nalu& nalu, 
                         H266SliceHeader* slice_header,
                         const H266PictureHeader* picture_header);
 */
  /// Parse NAL units from a buffer and extract their information
  /// @param data Buffer containing NAL units
  /// @param size Size of the buffer
  /// @param nal_units Output vector to store parsed NAL unit information
  /// @return true on success, false otherwise

  bool ParseNalUnits(const uint8_t* data,
                     size_t size,
                     std::vector<NalUnit>* nal_units);

  /// Parses a PPS element.
  Result ParsePps(const Nalu& nalu, int* pps_id);
  
  /// Parses a SPS element.
  Result ParseSps(const Nalu& nalu, int* sps_id);
  
  /// Parses a VPS element.
  Result ParseVps(const Nalu& nalu, int* vps_id);
  
  /// Parses an APS element.
  Result ParseAps(const Nalu& nalu, int* aps_id, int* aps_type);

  /// Parses a Picture Header.
  Result ParsePictureHeader(const Nalu& nalu, H266PictureHeader* picture_header);

#if 0   
//future update perhaps 
  /// Parses a DCI (Decoding Capability Information) element.
  Result ParseDci(const Nalu& nalu, H266DecodingCapabilityInfo* dci);
  
  /// Parses an OPI (Operating Point Information) element.
  Result ParseOpi(const Nalu& nalu, H266OperatingPointInfo* opi);
  
  /// Parses an SEI message.
  Result ParseSei(const Nalu& nalu, H266SEIMessage* sei_msg);

#endif 

  /// @return a pointer to the PPS with the given ID, or NULL if none exists.
  const H266Pps* GetPps(int pps_id);
  
  /// @return a pointer to the SPS with the given ID, or NULL if none exists.
  const H266Sps* GetSps(int sps_id);
  
  /// @return a pointer to the VPS with the given ID, or NULL if none exists.
  const H266Vps* GetVps(int vps_id);
  
  /// @return a pointer to the APS with the given ID, or NULL if none exists.
  const H266Aps* GetAps(int aps_id);

 private:
  Result Vui_Payload(int max_num_sub_layers_minus1,
                            H26xBitReader* br,
                            H266VuiParameters* vui);

  Result Ref_Pic_List_Struct(int listIdx, int rplsIdx,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266ReferencePicListStruct* rpls);

  Result H266Parser::GetGeneralTimingHrdParameters(GeneralTimingHrdParameters *time,
                                      H26xBitReader* br);                          
  
  Result Ols_Timing_Hrd_parameters(int firstsublayer, int sps_max_sublayers_minus1,
                            const H266Sps& sps, 
                            H26xBitReader* br,
                            H266OlsTimingHrdParameters* olf);
                            

  Result ParseProfileTierLevel(bool profile_tier_present,
                              int max_num_sub_layers_minus1,
                              H26xBitReader* br);
  Result ParseProfileTierLevel(bool profile_tier_present,
                               int max_num_sub_layers_minus1,
                               H26xBitReader* br,
                               H266ProfileTierLevel* profile_tier_level);

#if 0   
//future update perhaps
  Result ParseReferencePictureList(const H266Sps& sps,
                                  const H266Pps& pps,
                                  H26xBitReader* br,
                                  H266SliceHeader* slice_header);
#endif

  Result SkipScalingListData(H26xBitReader* br);
#if 0   
//future update perhaps
  Result SkipAlfData(H26xBitReader* br);
 
  Result SkipLmcsData(H26xBitReader* br);
#endif

  Result ByteAlignment(H26xBitReader* br);
#if 0   
//future update perhaps
  // H.266 specific parsing helpers
  Result ParseOlsIds(H26xBitReader* br, std::vector<int>* ols_ids);
  Result ParseDpbParameters(int max_sublayers_minus1,
                           bool sublayer_info_flag,
                           H26xBitReader* br);
  Result ParseGeneralConstraintsInfo(H26xBitReader* br);
#endif


  typedef std::map<int, std::unique_ptr<H266Vps>> VpsById;
  typedef std::map<int, std::unique_ptr<H266Sps>> SpsById;
  typedef std::map<int, std::unique_ptr<H266Pps>> PpsById;
  typedef std::map<int, std::unique_ptr<H266Aps>> ApsById;

  VpsById active_vpses_;
  SpsById active_spses_;
  PpsById active_ppses_;
  ApsById active_apses_;

  DISALLOW_COPY_AND_ASSIGN(H266Parser);
};

// Forward declarations for H.266 specific structures
struct H266DecodingCapabilityInfo;
struct H266OperatingPointInfo;
struct H266SEIMessage;

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_H266_PARSER_H_