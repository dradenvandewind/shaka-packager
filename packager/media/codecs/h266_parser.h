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

  // Incomplete: many more H.266 specific fields...
};

struct H266Sps {
  H266Sps();
  ~H266Sps();

  int GetPicSizeInCtbsY() const;
  int GetChromaArrayType() const;

  int sps_seq_parameter_set_id = 0;
  int vps_id = 0;  // H.266 uses vps_id directly in SPS
  int max_sublayers_minus1 = 0;
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

  int vps_video_parameter_set_id = 0;
  int vps_max_layers_minus1 = 0;
  int vps_max_sublayers_minus1 = 0;

  // Timing info in VPS (H.266 specific)
  bool vps_timing_info_present_flag = false;
  long vps_num_units_in_tick = 0;
  long vps_time_scale = 0;

  // General constraints
  bool vps_each_layer_is_an_ols_flag = false;
  int vps_ols_mode_idc = 0;

  // Output layer sets
  int vps_num_output_layer_sets_minus1 = 0;
  int vps_num_ptls_minus1 = 0;

  // Profile tier level
  int general_profile_tier_level_data[kMaxNumProfileTierLevels]
                                     [kGeneralProfileTierLevelBytes];

  // Layer sets
  int vps_num_layer_sets_minus1 = 0;
  int vps_max_layer_id = 0;

  // Scalability info
  int scalability_type = kNone;

  // H.266 specific: OPI (Operating Point Information) support
  bool vps_opi_present_flag = false;

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
};

/// A class to parse H.266 streams.
class H266Parser {
 public:
  enum Result {
    kOk,
    kInvalidStream,      // error in stream
    kUnsupportedStream,  // stream not supported by the parser
    kEOStream,           // end of stream
  };

  H266Parser();
  ~H266Parser();

  /// Parses a video slice header.
  Result ParseSliceHeader(const Nalu& nalu, H266SliceHeader* slice_header);
  
  /// Parses a slice header with picture header context
  Result ParseSliceHeader(const Nalu& nalu, 
                         H266SliceHeader* slice_header,
                         const H266PictureHeader* picture_header);

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
  Result ParseVuiParameters(int max_num_sub_layers_minus1,
                            H26xBitReader* br,
                            H266VuiParameters* vui);

  Result ParseProfileTierLevel(bool profile_tier_present,
                              int max_num_sub_layers_minus1,
                              H26xBitReader* br);
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