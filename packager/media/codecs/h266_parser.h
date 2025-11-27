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
#include <optional>

#include <packager/macros/classes.h>
#include <packager/media/codecs/h26x_bit_reader.h>

namespace shaka {
namespace media {

class Nalu;

enum H266SliceType { kVvcBSlice = 0, kVvcPSlice = 1, kVvcISlice = 2 };

const int kVvcMaxRefPicSetCount = 16;

// H.266 profile_tier_level structure is more complex than H.265
const int kVvcGeneralProfileTierLevelBytes = 12;
const int kVvcMaxNumProfileTierLevels = 8;  // Increased for H.266
const int kVvcMaxLayers = 8;  // Increased for H.266 scalability
const int kVvcMaxScalabilityTypes = 8;
const int kVvcMaxLayerIdPlus1 = 64;
const int kVvcMaxLayerSets = 16;
const int kVvcMaxOuputLayerSets = kVvcMaxLayerSets;

const int kVvcInvalidId = -1;

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
  int delta_poc_s0[kVvcMaxRefPicSetCount];
  int delta_poc_s1[kVvcMaxRefPicSetCount];
  bool used_by_curr_pic_s0[kVvcMaxRefPicSetCount];
  bool used_by_curr_pic_s1[kVvcMaxRefPicSetCount];

  int num_negative_pics = 0;
  int num_positive_pics = 0;
  int num_delta_pocs = 0;
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
struct H266DPB_Parameters{
  std::vector<int> dpb_max_dec_pic_buffering_minus1;
  std::vector<int> dpb_max_num_reorder_pics;
  std::vector<int> dpb_max_latency_increase_plus1;

};

struct H266VuiParameters {
  H266VuiParameters();
  ~H266VuiParameters();
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
  bool vui_progressive_source_flag = false;
  bool vui_interlaced_source_flag = false;
  bool vui_non_packed_constraint_flag = false;
  bool vui_non_projected_constraint_flag = false;
  bool vui_aspect_ratio_info_present_flag = false;
  bool vui_aspect_ratio_constant_flag = false;
  int vui_aspect_ratio_idc = 0;
  int vui_sar_width = 0;
  int vui_sar_height = 0;
  bool vui_overscan_info_present_flag = false;
  bool vui_overscan_appropriate_flag = false;

  bool vui_colour_description_present_flag = false;
  int vui_colour_primaries = 0;
  int vui_transfer_characteristics = 0;
  int vui_matrix_coeffs = 0;
  //int vui_full_range_flag;

  //bool vui_chroma_loc_info_present_flag;
  //u_int vui_chroma_sample_loc_type_frame;
  //u_int vui_chroma_sample_loc_type_top_field;
  //u_int vui_chroma_sample_loc_type_bottom_field;
  // Incomplete...
};
struct H266GeneralConstraintsInfo{
bool gci_present_flag = false;
/* general */
bool gci_intra_only_constraint_flag = false;
bool gci_all_layers_independent_constraint_flag = false;
bool gci_one_au_only_constraint_flag = false;
/* picture format */
int gci_sixteen_minus_max_bitdepth_constraint_idc = 0; //4 bits
int gci_three_minus_max_chroma_format_constraint_idc = 0;//2 bits
/* NAL unit type related */
bool gci_no_mixed_nalu_types_in_pic_constraint_flag = false;
bool gci_no_trail_constraint_flag = false;
bool gci_no_stsa_constraint_flag = false;
bool gci_no_rasl_constraint_flag = false;
bool gci_no_radl_constraint_flag = false;
bool gci_no_idr_constraint_flag = false;
bool gci_no_cra_constraint_flag = false;
bool gci_no_gdr_constraint_flag = false;
bool gci_no_aps_constraint_flag = false;
bool gci_no_idr_rpl_constraint_flag = false;
/* tile, slice, subpicture partitioning */
bool gci_one_tile_per_pic_constraint_flag = false;
bool gci_pic_header_in_slice_header_constraint_flag = false;
bool gci_one_slice_per_pic_constraint_flag = false;
bool gci_no_rectangular_slice_constraint_flag = false;
bool gci_one_slice_per_subpic_constraint_flag = false;
bool gci_no_subpic_info_constraint_flag = false;
/* CTU and block partitioning */
int gci_three_minus_max_log2_ctu_size_constraint_idc = 0; //2 bits
bool gci_no_partition_constraints_override_constraint_flag = false;
bool gci_no_mtt_constraint_flag = false;
bool gci_no_qtbtt_dual_tree_intra_constraint_flag = false;
/* intra */
bool gci_no_palette_constraint_flag = false;
bool gci_no_ibc_constraint_flag = false;
bool gci_no_isp_constraint_flag = false;
bool gci_no_mrl_constraint_flag = false;
bool gci_no_mip_constraint_flag = false;
bool gci_no_cclm_constraint_flag = false;
/* inter */
bool gci_no_ref_pic_resampling_constraint_flag = false;
bool gci_no_res_change_in_clvs_constraint_flag = false;
bool gci_no_weighted_prediction_constraint_flag = false;
bool gci_no_ref_wraparound_constraint_flag = false;
bool gci_no_temporal_mvp_constraint_flag = false;
bool gci_no_amvr_constraint_flag = false;
bool gci_no_bdof_constraint_flag = false;
bool gci_no_smvd_constraint_flag = false;
bool gci_no_dmvr_constraint_flag = false;
bool gci_no_mmvd_constraint_flag = false;
bool gci_no_affine_motion_constraint_flag = false;
bool gci_no_prof_constraint_flag = false;
bool gci_no_bcw_constraint_flag = false;
bool gci_no_ciip_constraint_flag = false;
bool gci_no_gpm_constraint_flag = false;
/* transform, quantization, residual */
bool gci_no_luma_transform_size_64_constraint_flag = false;
bool gci_no_transform_skip_constraint_flag = false;
bool gci_no_bdpcm_constraint_flag = false;
bool gci_no_mts_constraint_flag = false;
bool gci_no_lfnst_constraint_flag = false;
bool gci_no_joint_cbcr_constraint_flag = false;
bool gci_no_sbt_constraint_flag = false;
bool gci_no_act_constraint_flag = false;
bool gci_no_explicit_scaling_list_constraint_flag = false;
bool gci_no_dep_quant_constraint_flag = false;
bool gci_no_sign_data_hiding_constraint_flag = false;
bool gci_no_cu_qp_delta_constraint_flag = false;
bool gci_no_chroma_qp_offset_constraint_flag = false;
/* loop filter */
bool gci_no_sao_constraint_flag = false;
bool gci_no_alf_constraint_flag = false;
bool gci_no_ccalf_constraint_flag = false;
bool gci_no_lmcs_constraint_flag = false;
bool gci_no_ladf_constraint_flag = false;
bool gci_no_virtual_boundaries_constraint_flag = false;
int gci_num_additional_bits = 0; //8 bits
bool gci_all_rap_pictures_constraint_flag = false;
bool gci_no_extended_precision_processing_constraint_flag = false;
bool gci_no_ts_residual_coding_rice_constraint_flag = false;
bool gci_no_rrc_rice_extension_constraint_flag = false;
bool gci_no_persistent_rice_adaptation_constraint_flag = false;
bool gci_no_reverse_last_sig_coeff_constraint_flag = false;

};

struct H266ProfileTierLevel{
  H266ProfileTierLevel();
  ~H266ProfileTierLevel();

  int general_profile_idc = 0;
  bool general_tier_flag = 0;
  int general_level_idc = 0;
  bool ptl_frame_only_constraint_flag = false;
  bool ptl_multilayer_enabled_flag = false;

  H266GeneralConstraintsInfo gci;

  std::vector <bool> ptl_sublayer_level_present_flag;
  std::vector <bool> sublayer_level_idc;
  int ptl_num_sub_profiles = 0;
  std::vector <uint32_t> general_sub_profile_idc;
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
  int pps_cu_chroma_qp_offset_list_len_minus1 = 0; 
  int pps_chroma_qp_offset_list_len_minus1 = 0;
  std::vector<int> pps_qp_offset_list; //256 not sure need check
  std::vector<int> pps_cr_qp_offset_list;
  std::vector<int> pps_joint_cbcr_qp_offset_list;
  std::vector<int> pps_cb_qp_offset_list;



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
  bool pps_extension_data_flags = false;
  bool pps_extension_data_flag = false;
  int CtbSizeY = 0;


  //we need back up for slice_parsing
  uint32_t NumTileColumns = 0;  
  uint32_t NumTileRows = 0;
  uint32_t NumTilesInPic = 0;



};
// Dans h266_parser.h, ajouter :
struct GeneralTimingHrdParameters {
  GeneralTimingHrdParameters();
  ~GeneralTimingHrdParameters();

    uint32_t num_units_in_tick = 0;
    uint32_t time_scale = 0;
    bool general_nal_hrd_params_present_flag = false;
    bool general_vcl_hrd_params_present_flag = false;
    bool general_same_pic_timing_in_all_ols_flag = false;
    bool general_du_hrd_params_present_flag = false;
    uint8_t tick_divisor_minus2 = 0;
    uint8_t bit_rate_scale = 0;
    uint8_t cpb_size_scale = 0;
    uint8_t cpb_size_du_scale = 0;
    int hrd_cpb_cnt_minus1 = 0;
};

// struct GeneralTimingHrdParameters{
//   uint32_t num_units_in_tick;
//   uint32_t time_scale;
//   bool general_nal_hrd_params_present_flag;
//   bool general_vcl_hrd_params_present_flag;
//   bool general_same_pic_timing_in_all_ols_flag;
//   bool general_du_hrd_params_present_flag;
//   uint8_t tick_divisor_minus2;
//   int bit_rate_scale;
//   int cpb_size_scale;
//   int cpb_size_du_scale;
//   int hrd_cpb_cnt_minus1;
// };

/* 
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
 */
struct H266RefPicListEntry {
  H266RefPicListEntry();
  ~H266RefPicListEntry();
  
    bool inter_layer_ref_pic_flag = false;
    bool st_ref_pic_flag = false;
    int abs_delta_poc_st = 0;
    bool strp_entry_sign_flag = false;
    uint32_t rpls_poc_lsb_lt = 0;
    int ilrp_idx = 0;
};

struct H266ReferencePicListStruct {
  H266ReferencePicListStruct();
  ~H266ReferencePicListStruct();

    //int num_ref_entries = 0;
    std::vector<std::vector<int>> num_ref_entries;
    std::vector<std::vector<bool>> ltrp_in_header_flag;
    std::vector<H266RefPicListEntry> entries;
    std::vector<int> NumRefIdxActive;

    /*     test pour compile */
  //std::vector<std::vector<std::vector<bool>>> ltrp_in_header_flag;
  std::vector<std::vector<std::vector<bool>>> inter_layer_ref_pic_flag;
  std::vector<std::vector<std::vector<bool>>> st_ref_pic_flag;
  std::vector<std::vector<std::vector<int>>> abs_delta_poc_st;
  std::vector<std::vector<std::vector<bool>>> strp_entry_sign_flag;
  std::vector<std::vector<std::vector<int>>> rpls_poc_lsb_lt;
  std::vector<std::vector<std::vector<int>>> ilrp_idx;


};

struct H266ReferencePicList{
  H266ReferencePicList();
  ~H266ReferencePicList();

  std::vector <bool> rpl_sps_flag;
  std::vector <int> rpl_idx;
  std::vector<std::vector<std::vector<int>>> poc_lsb_lt;
  //std::vector<std::vector<std::vector<bool>>> delta_poc_msb_cycle_present_flag;
  std::vector<std::vector<bool>> delta_poc_msb_cycle_present_flag;
  //std::vector<std::vector<std::vector<int>>> delta_poc_msb_cycle_lt;
  std::vector<std::vector<int>> delta_poc_msb_cycle_lt;
  std::optional<H266ReferencePicListStruct> reference_pic_list;

  //aditionnal variables
  std::vector<std::vector<int>> NumLtrpEntries;
  std::vector<std::vector<std::vector<bool>>> inter_layer_ref_pic_flag;
  std::vector<std::vector<std::vector<bool>>> st_ref_pic_flag;
  std::vector<std::vector<std::vector<bool>>> ltrp_in_header_flag;
  std::vector<std::vector<int>> num_ref_entries;
  std::vector <int> RplsIdx;
};

struct H266PredWeightTable{
  H266PredWeightTable();
  ~H266PredWeightTable();

  int luma_log2_weight_denom = 0;
  int delta_chroma_log2_weight_denom = 0;
  int num_l0_weights = 0;
  std::vector<bool> luma_weight_l0_flag;
  std::vector<bool> chroma_weight_l0_flag;
  std::vector<int> delta_luma_weight_l0;
  std::vector<int> luma_offset_l0;
  std::vector<int> delta_chroma_weight_l0;
  std::vector<int> delta_chroma_offset_l0;
  int num_l1_weights;

  std::vector<bool> luma_weight_l1_flag;
  std::vector<bool> chroma_weight_l1_flag;
  std::vector<int> delta_luma_weight_l1;
  std::vector<int> luma_offset_l1;

  std::vector<std::vector<int>> delta_chroma_weight_l1;
  std::vector<std::vector<int>> delta_chroma_offset_l1;

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
  H266ProfileTierLevel sps_profile_level;
  bool sps_gdr_enabled_flag = false;
  bool sps_ref_pic_resampling_enabled_flag = false;
  bool sps_res_change_in_clvs_allowed_flag = false;
  int sps_pic_width_in_luma_samples = 0;
  int sps_pic_width_max_in_luma_samples = 0;
  int sps_pic_height_max_in_luma_samples = 0;
  int sps_pic_height_in_luma_samples = 0;

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
  

  bool sps_subpic_same_size_flag = false;
  std::vector<int> sps_subpic_top_left_x;
  std::vector<int> sps_subpic_top_left_y;
  std::vector<int> sps_subpic_width_minus1;
  std::vector<int> sps_subpic_height_minus1;
  std::vector<bool> sps_subpic_treated_as_pic_flag;
  std::vector<bool> sps_loop_filter_across_subpic_enabled_flag;
  int sps_subpic_id_len_minus1 = 0;
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
  int sps_max_mtt_hierarchy_depth_intra_slice_chroma = 0;
  int sps_log2_diff_min_qt_min_cb_inter_slice = 0;
  int sps_max_mtt_hierarchy_depth_inter_slice = 0;

  int sps_log2_diff_max_bt_min_qt_inter_slice = 0;
  int sps_log2_diff_max_tt_min_qt_inter_slice = 0;
  int sps_log2_diff_max_bt_min_qt_intra_slice_chroma = 0;
  bool sps_max_luma_transform_size_64_flag = false;
  bool sps_transform_skip_enabled_flag = false;

  int sps_log2_transform_skip_max_size_minus2 = 0;
  bool sps_bdpcm_enabled_flag = 0;
  
  bool sps_mts_enabled_flag = false;
  bool sps_explicit_mts_intra_enabled_flag = false;
  bool sps_explicit_mts_inter_enabled_flag = false;
  bool sps_lfnst_enabled_flag = false;
  bool sps_joint_cbcr_enabled_flag = false;
  bool sps_same_qp_table_for_chroma_flag = false;

  std::vector <uint32_t> sps_qp_table_start_minus26;//not sure need check
  std::vector <uint32_t> sps_num_points_in_qp_table_minus1;//not sure need check
  
  std::vector<std::vector<std::vector<int>>> sps_delta_qp_in_val_minus1;//not sure need check
  std::vector<std::vector<std::vector<int>>> sps_delta_qp_diff_val;//not sure need check

  bool sps_sao_enabled_flag = false;
  bool sps_ccalf_enabled_flag = false;
  bool sps_alf_enabled_flag = false;
  bool sps_lmcs_enabled_flag = false;
  bool sps_weighted_pred_flag = false;
  bool sps_weighted_bipred_flag = false;
  bool sps_long_term_ref_pics_flag = false;
  //std::optional<
   H266ReferencePicListStruct pic;
  bool sps_inter_layer_prediction_enabled_flag = false;
  bool sps_idr_rpl_present_flag = false;
  bool sps_rpl1_same_as_rpl0_flag = false;
  std::vector <int> sps_num_ref_pic_lists;
  //std::optional<
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

  std::vector <int> sps_virtual_boundary_pos_x_minus1;//i = sps_num_ver_virtual_boundaries
  int sps_num_hor_virtual_boundaries = 0;

  std::vector <int> sps_virtual_boundary_pos_y_minus1;//i = sps_num_hor_virtual_boundaries  

  bool sps_timing_hrd_params_present_flag = false;
  //GeneralTimingHrdParameters general_timing_hrd_parameters = null;
  std::optional<GeneralTimingHrdParameters> general_timing_hrd_parameters;
  std::optional<H266OlsTimingHrdParameters> ols_parameters;
  //std::optional<H266OlsTimingHrdParameters> general_timing_hrd_parameters;
  bool sps_sublayer_cpb_params_present_flag = false;


  bool sps_field_seq_flag = false;
  bool sps_vui_parameters_present_flag = false;

  int sps_vui_payload_size_minus1 = 0;  
  H266VuiParameters vui_parameters;

  bool sps_vui_alignment_zero_bits = false;
  bool sps_extension_flag = false;

  bool sps_range_extension_flag = false;
  bool sps_extension_7bits_flag = false;
  int sps_extension_7bits = 0;
  bool sps_extension_data_flag = false;
 
// sp_range_extension  
// todo make struct and funct
bool sps_extended_precision_flag = false;
bool sps_ts_residual_coding_rice_present_in_sh_flag = false;
bool sps_rrc_rice_extension_flag = false;
bool sps_persistent_rice_adaptation_enabled_flag = false;
bool sps_reverse_last_sig_coeff_enabled_flag  = false;


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

  // Strong intra smoothing
  bool sps_strong_intra_smoothing_enabled_flag = false;
  // OLS timing hrd parameters
  //H266OlsTimingHrdParameters* ols_parameters = null;
  //std::optional<H266OlsTimingHrdParameters> ols_parameters;


  // VUI parameters
  bool vui_parameters_present = false;
  //H266VuiParameters vui_parameters;


  // H.266 specific tools

  // Sub-picture and scalability

  // Incomplete: many more H.266 specific fields...
};

struct H266RepFormat {
  H266RepFormat();
  ~H266RepFormat();
  
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

  int vps_video_parameter_set_id = 0; //4 bits
  //int vps_max_layers_minus1;
  uint32_t vps_max_layers_minus1 = 0; //6 bits
  int vps_max_sublayers_minus1 = 0; //3 bits
  bool vps_default_ptl_dpb_hrd_max_tid_flag = false; //bool
  bool vps_all_independent_layers_flag = false; //bool

  std::vector<uint8_t> vps_layer_id; // 6 bits each, size = vpsMaxLayersMinus1 + 1
  std::vector<bool> vps_independent_layer_flag;
  
  std::vector<bool> vps_max_tid_ref_present_flag;
  std::vector<std::vector<bool>>vps_direct_ref_layer_flag;
  std::vector<std::vector<int>> vps_max_tid_il_ref_pics_plus1;
  
  //bool vps_each_layer_is_an_ols_flag = false;
  //int vps_ols_mode_idc = 0;
  int vps_num_output_layer_sets_minus2 = 0;

  std::vector<std::vector<bool>> vps_ols_output_layer_flag;

  //int vps_num_ptls_minus1;
  std::vector<bool> vps_pt_present_flag;
  std::vector<int> vps_ptl_max_tid;

  bool vps_ptl_alignment_zero_bit = false;
  std::optional<H266ProfileTierLevel> vps_ptl;
  std::vector<int> vps_ols_ptl_idx;
 
  int vps_num_dpb_params_minus1 = 0;
  bool vps_sublayer_dpb_params_present_flag = false;
  std::vector<int> vps_dpb_max_tid;
  std::optional<H266DPB_Parameters> vps_dpd;

  //dpb_parameters  params;

  std::vector<int> vps_ols_dpb_pic_width;
  std::vector<int> vps_ols_dpb_pic_height;
  std::vector<int> vps_ols_dpb_chroma_format;
  std::vector<int> vps_ols_dpb_bitdepth_minus8;

  std::vector<int> vps_ols_dpb_params_idx;
  bool vps_timing_hrd_params_present_flag = false;
  //general_timing_hrd_parameters()
  std::optional<GeneralTimingHrdParameters> vps_general_timing_hrd_parameters;
  bool vps_sublayer_cpb_params_present_flag = false;
  int vps_num_ols_timing_hrd_params_minus1 = 0;
  //general_timing_hrd_parameters( )

  std::vector<int> vps_hrd_max_tid;

  //ols_timing_hrd_parameters
  std::optional<H266OlsTimingHrdParameters> vps_ols_parameters;

  std::vector<int> vps_ols_timing_hrd_idx;
  bool vps_extension_flag = false;
  bool vps_extension_data_flag = false;

  //extra variales 
  int TotalNumOlss = 0;
  std::vector<std::vector<int>> LayerIdInOls;
  std::vector<int> NumLayersInOls;
  //int NumMultiLayerOlss = 0;

  std::vector<std::vector<bool>> layerIncludedInOlsFlag;
  std::vector<int>  MultiLayerOlsIdx;
  int VpsNumDpbParams = 0;

  std::vector<int> NumOutputLayersInOls;
  std::vector<std::vector<int>> OutputLayerIdInOls;
  std::vector<std::vector<int>> NumSubLayersInLayerInOLS;
  std::vector<std::vector<int>> OutputLayerIdx;
  std::vector<int> LayerUsedAsOutputLayerFlag;

  std::vector<int> NumRefLayers;
  std::vector<std::vector<int>> ReferenceLayerIdx;
  std::vector<std::vector<bool>> dependencyFlag;
  int NumMultiLayerOlss = 0;

  std::vector <bool> LayerUsedAsRefLayerFlag;

  std::vector<std::vector<int>> DirectRefLayerIdx;
  std::vector<std::vector<int>> NumDirectRefLayers;
  std::vector<std::vector<bool>> vps_direct_dependency_flag;

  // Timing info in VPS (H.266 specific)
  bool vps_timing_info_present_flag = false;
  long vps_num_units_in_tick= 0;
  long vps_time_scale= 0;

  // General constraints
  bool vps_each_layer_is_an_ols_flag = false;
  int vps_ols_mode_idc = 0;

  // Output layer sets
  int vps_num_output_layer_sets_minus1= 0;
  int vps_num_ptls_minus1 = 0;

  // Profile tier level
  int general_profile_tier_level_data[kVvcMaxNumProfileTierLevels]
                                     [kVvcGeneralProfileTierLevelBytes];

  // Layer sets
  int vps_num_layer_sets_minus1= 0;
  int vps_max_layer_id = 0;

  // Scalability info
  int scalability_type = kNone;

  // H.266 specific: OPI (Operating Point Information) support
  bool vps_opi_present_flag = false;

  /*                */
  bool vps_default_output_layer_idc = false;
  //bool vps_all_independent_layers_flag;
  //std::vector<uint32_t> layer_id_included_flag;
  std::vector<bool> layer_id_included_flag;


  // Timing info
  bool vps_poc_proportional_to_timing_flag = false;
  uint32_t vps_num_ticks_poc_diff_one_minus1= 0;
  
  // Output layer sets
  uint32_t vps_num_output_layer_sets= 0;
  std::vector<std::vector<bool>> output_layer_flag;
  
  // Profile tier level
  H266ProfileTierLevel profile_tier_level;
  
  // Layer dependency
  //std::vector<std::vector<bool>> direct_dependency_flag;
  //std::vector<uint32_t> max_tid_ref_present_flag;
  

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
struct H266PictureHeaderStructure{
  //7.3.2.8 Picture header structure syntax
  
 bool ph_gdr_or_irap_pic_flag = false;
 bool ph_non_ref_pic_flag = false;
 bool ph_gdr_pic_flag = false;
 bool ph_inter_slice_allowed_flag = false;
 bool ph_intra_slice_allowed_flag = false;
 int ph_pic_parameter_set_id = 0;
 int ph_pic_order_cnt_lsb = 0;
 int ph_recovery_poc_cnt = 0;
 std::vector <bool> ph_extra_bit;

 bool ph_poc_msb_cycle_present_flag = false;
 int ph_poc_msb_cycle_val = 0;
 bool ph_alf_enabled_flag = false;
 int ph_num_alf_aps_ids_luma = 0;
 std::vector <int> ph_alf_aps_id_luma;

 bool ph_alf_cb_enabled_flag = false;
 bool ph_alf_cr_enabled_flag = false;
 bool ph_alf_aps_id_chroma = false;

 bool ph_alf_cc_cr_enabled_flag = false;
 bool ph_alf_cc_cb_enabled_flag = false;
 int ph_alf_cc_cb_aps_id = 0;
 int ph_alf_cc_cr_aps_id = 0;
 bool ph_lmcs_enabled_flag = false;
 int ph_lmcs_aps_id = 0;
 bool ph_chroma_residual_scale_flag = false;
 bool ph_explicit_scaling_list_enabled_flag = false;
 int ph_scaling_list_aps_id = 0;

 bool ph_virtual_boundaries_present_flag = false;
 int ph_num_ver_virtual_boundaries = 0;
 std::vector<int> ph_virtual_boundary_pos_x_minus1;
 int ph_num_hor_virtual_boundaries = 0;
 std::vector <int> ph_virtual_boundary_pos_y_minus1;
 bool ph_pic_output_flag = false;
 //ref_pic_lists
 std::optional<H266ReferencePicList> rpl;

 bool ph_partition_constraints_override_flag = false;
 int ph_log2_diff_min_qt_min_cb_intra_slice_luma = 0;
 int ph_max_mtt_hierarchy_depth_intra_slice_luma = 0;
 int ph_log2_diff_max_bt_min_qt_intra_slice_luma = 0;
 int ph_log2_diff_max_tt_min_qt_intra_slice_luma = 0;
 int ph_log2_diff_min_qt_min_cb_intra_slice_chroma = 0;
 int ph_max_mtt_hierarchy_depth_intra_slice_chroma = 0;
 int ph_log2_diff_max_bt_min_qt_intra_slice_chroma = 0;
 int ph_log2_diff_max_tt_min_qt_intra_slice_chroma = 0;
 int ph_cu_qp_delta_subdiv_intra_slice = 0;
 int ph_cu_chroma_qp_offset_subdiv_intra_slice = 0;
 int ph_log2_diff_min_qt_min_cb_inter_slice = 0;
 int ph_max_mtt_hierarchy_depth_inter_slice = 0;
 int ph_log2_diff_max_bt_min_qt_inter_slice = 0;
 int ph_log2_diff_max_tt_min_qt_inter_slice = 0;
 int ph_cu_qp_delta_subdiv_inter_slice = 0;
 int ph_cu_chroma_qp_offset_subdiv_inter_slice = 0;


 bool ph_temporal_mvp_enabled_flag = false;
 bool ph_collocated_from_l0_flag = false;
 int ph_collocated_ref_idx = 0;
 bool ph_mmvd_fullpel_only_flag = false;

 bool ph_mvd_l1_zero_flag = false;
 bool ph_bdof_disabled_flag = false;

 bool ph_dmvr_disabled_flag = false;

 std::optional <H266PredWeightTable> p_pwt;

 bool ph_prof_disabled_flag = false;


 int ph_qp_delta = 0;

 bool ph_joint_cbcr_sign_flag = false;

 bool ph_sao_luma_enabled_flag = false;
 bool ph_sao_chroma_enabled_flag = false;

 bool ph_deblocking_params_present_flag = false;
 bool ph_deblocking_filter_disabled_flag = false;
 int ph_luma_beta_offset_div2 = 0;
 int ph_luma_tc_offset_div2 = 0;
 int ph_cb_beta_offset_div2 = 0;
 int ph_cb_tc_offset_div2 = 0;
 int ph_cr_beta_offset_div2 = 0;
 int ph_cr_tc_offset_div2 = 0;



 bool ph_extension_length = false;
 std::vector <int> ph_extension_data_byte; //u_int8_t


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

struct H266PictureHeaderRbsp{
  H266PictureHeaderRbsp();
  ~H266PictureHeaderRbsp();
  std::optional<H266PictureHeaderStructure> phs;

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
  std::optional<H266PictureHeaderStructure> phs;

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
  std::optional<H266PredWeightTable> pwt;

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
int CurrSubpicIdx = 0;
std::vector<int> SubpicIdVal;
std::vector<int> NumSlicesInSubpic;
std::vector<int> SubpicLevelSliceIdx;
std::vector<int> SubpicIdxForSlice;
std::vector<std::vector<int>> CtbAddrInSlice;
int PicWidthInCtbsY = 0;
int PicHeightInCtbsY = 0;

std::vector <bool> subpicHeightLessThanOneTileFlag;
std::vector <uint32_t> ctbToTileColIdx;
std::vector <uint32_t> ctbToTileRowIdx;




std::vector <int> SubpicHeightInTiles;
std::vector <int> SubpicWidthInTiles;
std::vector <uint32_t> TileColBdVal;
std::vector <uint32_t> TileRowBdVal;
std::vector <int>  NumCtusInSlice;
std::vector <int> SliceTopLeftTileIdx;
std::vector <int> sliceWidthInTiles;
std::vector <int> sliceHeightInTiles;
std::vector <int> NumSlicesInTile;

std::vector <uint32_t> ColWidthVal;
std::vector <uint32_t> RowHeightVal;
int NumExtraShBits = 0;
int NumEntryPoints = 0;
int NumCtusInCurrSlice = 0;
std::optional<H266ReferencePicList> rpl;

std::vector<int> CtbAddrInCurrSlice;
std::vector<uint32_t> CtbToTileRowBd;
std::vector<uint32_t> CtbToTileColBd;
int sh_luma_beta_offset_div2 = 0;
int sh_luma_tc_offset_div2 = 0;
std::vector<int> NumRefIdxActive;

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
  // Parse picture header rbsp
  Result ParsePictureHeaderRbsp(const Nalu& nalu,H266PictureHeaderRbsp *pictureheaderrbsp);


  /// Parses a slice header with picture header context
  
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

  Result ParsePictureHeaderStructure(const Nalu& nalu,
                                                  H266PictureHeaderStructure* phs);

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

  std::vector<const H266Pps*> GetPpsForSps(int sps_id);
  const H266Pps* GetFirstPpsForSps(int sps_id);

  std::vector<const H266Sps*> GetSpsForVps(int vps_id);
  const H266Sps* GetFirstSpsForVps(int vps_id);

  std::vector<const H266Pps*> GetPpsFromSps(int sps_id);
  std::vector<const H266Pps*> GetPpsFromVps(int vps_id);

  std::vector<const H266Sps*> GetSpsFromVps(int vps_id);
  std::vector<const H266Vps*> GetVpsFromSps(int sps_id);

  const H266Sps* GetFirstSpsFromPps(int pps_id);
  const H266Vps* GetFirstVpsFromPps(int pps_id);

  const H266Vps* GetFirstVpsFromSps(int sps_id);
  const H266Pps* GetFirstPpsFromVps(int vps_id);

  const H266Vps* GetFirstVps();
  const H266Sps* GetFirstSps();
  const H266Pps* GetFirstPps();


  Result Vui_Payload(int max_num_sub_layers_minus1,
                     H26xBitReader* br,
                     H266VuiParameters* vui);

  Result Ref_Pic_List_Struct(int listIdx,
                             int rplsIdx,
                             const H266Sps& sps,
                             H26xBitReader* br,
                             H266ReferencePicListStruct* rpls);

  Result Ref_Pic_List(const H266Sps& sps, const H266Pps& pps,
                            H26xBitReader* br,
                            H266ReferencePicList* rpl);
  
  Result PredWeightTable( const H266Sps& sps, const H266Pps& pps,
                          H26xBitReader* br,
                          H266ReferencePicList *rpl,
                          H266PredWeightTable *pwt,
                          int numweightsw0);
  Result Ols_Timing_Hrd_parameters(int firstsublayer, int sps_max_sublayers_minus1,
                            const H266Sps& sps, 
                            H26xBitReader* br,
                            H266OlsTimingHrdParameters* olf);     

  Result dpb_parameters( int MaxSubLayersMinus1, int subLayerInfoFlag ,
                          H266DPB_Parameters* dpd,
                          H26xBitReader* br);
  Result ParseGeneralConstraintsInfo(H266GeneralConstraintsInfo *gci,
                                                     H26xBitReader* br);

  Result GetGeneralTimingHrdParameters(GeneralTimingHrdParameters *time,
                                      H26xBitReader* br);                          
  

                            
  Result ParseProfileTierLevel(bool profile_tier_present,
                               int max_num_sub_layers_minus1,
                               H26xBitReader* br,
                               H266ProfileTierLevel* profile_tier_level);
  Result SkipScalingListData(H26xBitReader* br);
  Result ByteAlignment(H26xBitReader* br);

  Result rbsp_trailing_bits(H26xBitReader* br);

 private:
  
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