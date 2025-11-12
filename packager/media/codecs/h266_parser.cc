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
  LOG(INFO) << "Parsing H.266 PPS NALU";

  //warning  with }} for close

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;
  uint32_t NumTileColumns = 0;  
  uint32_t NumTileRows = 0;   
  uint32_t NumTilesInPic = 0; 


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
    //bckp up forr sps
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
        if( SliceTopLeftTileIdx[ i ] / NumTileColumns != NumTileRows-1 && ( pps->pps_tile_idx_delta_present_flag || SliceTopLeftTileIdx[ i ] % NumTileColumns = = 0 ) ){
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
      /*
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
      TRUE_OR_RETURN(br->ReadBool(&pps->pps_slice_header_extension_present_flag));

      TRUE_OR_RETURN(br->ReadBool(&pps->pps_extension_flag));
      if( pps->pps_extension_flag ) {
        //while( more_rbsp_data( ) ) {
          TRUE_OR_RETURN(br->ReadBool(&pps->pps_extension_data_flag));
        //}
      //rbsp_trailing_bits( )
      }
      */
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
              pps->pps_extension_data_flags.push_back(extension_data_flag);
          }
      }
      OK_OR_RETURN(ByteAlignment(br));    

        
    }
  }
}

  // This will replace any existing PPS instance.
  *pps_id = pps->pic_parameter_set_id;
  active_ppses_[*pps_id] = std::move(pps);

  return kOk;
}
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
  if( sps->sps_ptl_dpb_hrd_params_present_flag) {
    //todo
    //profile_tier_level( 1, sps_max_sublayers_minus1 )
    ParseProfileTierLevel(true, sps->max_sublayers_minus1, br, &sps->sps_profile_level);

  }
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_gdr_enabled_flag));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_ref_pic_resampling_enabled_flag));
  if( sps->sps_ref_pic_resampling_enabled_flag) {
    TRUE_OR_RETURN(br->ReadBool(&sps->sps_res_change_in_clvs_allowed_flag));
  }
  TRUE_OR_RETURN(br->ReadUE(&sps->sps_pic_width_max_in_luma_samples));
  TRUE_OR_RETURN(br->ReadUE(&sps->sps_pic_height_max_in_luma_samples));
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_conformance_window_flag));
  if (sps->sps_conformance_window_flag) {
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_left_offset));
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_right_offset));
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_top_offset));
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_bottom_offset));
  }
    TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_info_present_flag));
    if(sps->sps_subpic_info_present_flag) {
      TRUE_OR_RETURN(br->ReadUE(&sps->sps_num_subpics_minus1));
    }
    if(sps->sps_num_subpics_minus1 > 0) {
      TRUE_OR_RETURN(br->ReadBool(&sps->sps_independent_subpics_flag));
      TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_same_size_flag));
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
            }
            int tmp_sps_subpic_ctu_top_left_y = 0;
            if( i > 0 && sps->sps_pic_height_max_in_luma_samples > CtbSizeY ){
              TRUE_OR_RETURN(br->ReadBits(tmpHeightVal,&tmp_sps_subpic_ctu_top_left_y));  // u(v)  NOT SURE
              sps->sps_subpic_ctu_top_left_y.push_back(tmp_sps_subpic_ctu_top_left_y);
            }
            int tmp_sps_subpic_width_minus1 = 0;
            if( i < sps->sps_num_subpics_minus1 && sps->sps_pic_width_max_in_luma_samples > CtbSizeY ){              
              TRUE_OR_RETURN(br->ReadBits(tmpWidthVal,&tmp_sps_subpic_width_minus1));  // u(v)  NOT SURE
              sps->sps_subpic_width_minus1.push_back(tmp_sps_subpic_width_minus1);
            }
            int tmp_sps_subpic_height_minus1 = 0;
            if( i < sps->sps_num_subpics_minus1 && sps->sps_pic_height_max_in_luma_samples > CtbSizeY ){
              //sps_subpic_height_minus1[
              TRUE_OR_RETURN(br->ReadBits(tmpHeightVal,&tmp_sps_subpic_height_minus1));  // u(v)  NOT SURE
              sps->sps_subpic_ctu_top_left_y.push_back(tmp_sps_subpic_height_minus1);
            }
              
          }
          if( !sps->sps_independent_subpics_flag) {
            bool tmp_sps_subpic_treated_as_pic_flag;
            bool tmp_sps_loop_filter_across_subpic_enabled_flag;

          TRUE_OR_RETURN(br->ReadBool(&tmp_sps_subpic_treated_as_pic_flag));
          sps->sps_subpic_treated_as_pic_flag.push_back(tmp_sps_subpic_treated_as_pic_flag);
          TRUE_OR_RETURN(br->ReadBool(&tmp_sps_loop_filter_across_subpic_enabled_flag));
          sps->sps_loop_filter_across_subpic_enabled_flag.push_back(tmp_sps_loop_filter_across_subpic_enabled_flag);
          }
        } //for 
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_subpic_id_len_minus1));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_id_mapping_explicitly_signalled_flag));

        TRUE_OR_RETURN(br->ReadBool(&sps->sps_entry_point_offsets_present_flag));
        TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_log2_max_pic_order_cnt_lsb_minus4));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_poc_msb_cycle_flag));
        if(sps->sps_poc_msb_cycle_flag){
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_poc_msb_cycle_len_minus1));
        }
        TRUE_OR_RETURN(br->ReadBits(2,&sps->sps_num_extra_ph_bytes));
        bool tmp_sps_extra_sh_bit_present_flag = 0;
        for( int i = 0; i < (sps->sps_num_extra_sh_bytes * 8 ); i++ ){
          TRUE_OR_RETURN(br->ReadBool(&tmp_sps_extra_sh_bit_present_flag));
          sps->sps_extra_sh_bit_present_flag.push_back(tmp_sps_extra_sh_bit_present_flag);
        }
        if( sps->sps_ptl_dpb_hrd_params_present_flag ) {
          if( sps->max_sublayers_minus1 > 0 ){
              TRUE_OR_RETURN(br->ReadBool(&sps->sps_sublayer_dpb_params_flag));
              //TODO
             //dpb_parameters( sps_max_sublayers_minus1, sps_sublayer_dpb_params_flag )
          }
        }
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_min_luma_coding_block_size_minus2));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_partition_constraints_override_enabled_flag));
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma));
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_intra_slice_luma));
        if( sps->sps_max_mtt_hierarchy_depth_intra_slice_luma != 0 ) {
           TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma));
           TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma));
        }
        if( sps->sps_chroma_format_idc != 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_qtbtt_dual_tree_intra_flag));
        }
        if( sps->sps_qtbtt_dual_tree_intra_flag ) {

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma));
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma));
            if( sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma != 0 ) {
                TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma));
                TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma));
            }
        }


        TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_inter_slice));
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_inter_slice));
        if( sps->sps_max_mtt_hierarchy_depth_inter_slice != 0 ) {
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_inter_slice));
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_inter_slice));
        }
        if( pps->CtbSizeY > 32 )
        {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_max_luma_transform_size_64_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_transform_skip_enabled_flag));
        if( sps->sps_transform_skip_enabled_flag ) {
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_transform_skip_max_size_minus2));
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdpcm_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mts_enabled_flag));
        if( sps->sps_mts_enabled_flag ) {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_mts_intra_enabled_flag));
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_mts_inter_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_lfnst_enabled_flag));

        if( sps->sps_chroma_format_idc != 0 ) {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_joint_cbcr_enabled_flag));
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_same_qp_table_for_chroma_flag));
          int numQpTables = sps->sps_same_qp_table_for_chroma_flag ? 1 : ( sps->sps_joint_cbcr_enabled_flag ? 3 : 2 );
          int tmp_sps_qp_table_start_minus26 = 0;
          int tmp_sps_num_points_in_qp_table_minus1 = 0;
          for( int i = 0; i < numQpTables; i++ ) {

            TRUE_OR_RETURN(br->ReadSE(&tmp_sps_qp_table_start_minus26));
            sps->sps_qp_table_start_minus26.push_back(tmp_sps_qp_table_start_minus26);
            TRUE_OR_RETURN(br->ReadUE(&tmp_sps_num_points_in_qp_table_minus1));
            sps->sps_num_points_in_qp_table_minus1.push_back(tmp_sps_num_points_in_qp_table_minus1);
            
            int tmp_sps_delta_qp_in_val_minus1 = 0;
            int tmp_sps_delta_qp_diff_val = 0;
            for( int j = 0; j <= sps->sps_num_points_in_qp_table_minus1[ i ]; j++ ) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_delta_qp_in_val_minus1));
              sps->sps_delta_qp_in_val_minus1[i][j].push_back(tmp_sps_delta_qp_in_val_minus1);
              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_delta_qp_diff_val));
              sps->sps_delta_qp_diff_val[i][j].push_back(tmp_sps_delta_qp_diff_val);
            }
          }
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_sao_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_alf_enabled_flag));
        if( sps->sps_alf_enabled_flag && sps->sps_chroma_format_idc != 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_ccalf_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_lmcs_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_weighted_pred_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_weighted_bipred_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_long_term_ref_pics_flag));
        if( sps->sps_video_parameter_set_id > 0 ){
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_inter_layer_prediction_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_idr_rpl_present_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_long_term_ref_pics_flag));
        int tmp_sps_num_ref_pic_lists = 0;
        for( int i = 0; i < ( sps->sps_rpl1_same_as_rpl0_flag ? 1 : 2 ); i++ ) {

          TRUE_OR_RETURN(br->ReadUE(&tmp_sps_num_ref_pic_lists));
          sps->sps_num_ref_pic_lists.push_back(tmp_sps_num_ref_pic_lists);
          for( int j = 0; j < sps->sps_num_ref_pic_lists[ i ]; j++){
            Ref_Pic_List_Struct(i,j, *sps, br, &sps->reference_pic_list_struct);
          }
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_ref_wraparound_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_temporal_mvp_enabled_flag));
        if( sps->sps_temporal_mvp_enabled_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_sbtmvp_enabled_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_amvr_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdof_enabled_flag));
        if(sps->sps_bdof_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdof_control_present_in_ph_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_smvd_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_dmvr_enabled_flag));
        if(sps->sps_dmvr_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_dmvr_control_present_in_ph_flag));
        }
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mmvd_enabled_flag));
        if(sps->sps_mmvd_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&sps->sps_mmvd_fullpel_only_enabled_flag));
        }    

        TRUE_OR_RETURN(br->ReadUE(&sps->sps_six_minus_max_num_merge_cand));

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
            OK_OR_RETURN(GetGeneralTimingHrdParameters(&sps->general_timing_hrd_parameters,br));
          }
          
          if (sps->max_sublayers_minus1){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_sublayer_cpb_params_present_flag));
          }
          int firstSubLayer = sps->sps_sublayer_cpb_params_present_flag ? 0 : sps->max_sublayers_minus1;


            if (!sps->ols_parameters) {
              //sps->ols_parameters = std::make_unique<H266OlsTimingHrdParameters>();
              sps->ols_parameters.emplace();
            }

 

            Ols_Timing_Hrd_parameters(firstSubLayer, sps->max_sublayers_minus1,
                            *sps,
                            br,
                            sps->ols_parameters);
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
          //todo
          
          //vui_payload( sps->sps_vui_payload_size_minus1 + 1 )
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
        /*
        if(sps->sps_extension_7bits){
          while( more_rbsp_data() ){
            //sps_extension_data_flag
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_extension_data_flag));
        }
        rbsp_trailing_bits( );
        */
      
        
  
  // This will replace any existing SPS instance.
  *sps_id = sps->sps_seq_parameter_set_id;
  active_spses_[*sps_id] = std::move(sps);

  return kOk;
}

#if 0
H266Parser::Result H266Parser::ParseVps(const Nalu& nalu, int* vps_id) {
  DCHECK_EQ(Nalu::H266_VPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 VPS NALU";
  //video_parameter_set_rbsp( )  7.3.2.3

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *vps_id = -1;
  std::unique_ptr<H266Vps> vps(new H266Vps);

  TRUE_OR_RETURN(br->ReadBits(4, &vps->vps_video_parameter_set_id)); 
  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layers_minus1)); 
  TRUE_OR_RETURN(br->ReadBits(3, &vps->vps_max_sublayers_minus1)); 

  if (vps->vps_max_sublayers_minus1 > 0 && vps->vps_max_sublayers_minus1 >0 ) {
   TRUE_OR_RETURN(br->ReadBool(&vps->vps_default_ptl_dpb_hrd_max_tid_flag));
  }
  if(vps_max_layers_minus1 > 0) {
    //TODO layer_id_included_flag parsing per layer
    TRUE_OR_RETURN(br->ReadBool(&vps->vps_all_independent_layers_flag));
  }
  vps->vpsLayerId.clear();
  for(uint8_t i=0; i<= vps->vps_max_layers_minus1; i++) {
    uint8_t layerId;
    TRUE_OR_RETURN(br->ReadBits(6, &layerId)); // 6 bits

    TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_layer_id[i])); // 6 bits
    vps->vpsLayerId.push_back(layerId);
    if(i>0 && !vps->vps_all_independent_layers_flag) {
      //TODO parsing of layer_dependency_info( i )

    }

  }



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
    if (vps->direct_dependency_flag[layer_id][i]) {
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
  TRUE_OR_RETURN(br->ReadBits(32,&time->num_units_in_tick));
  TRUE_OR_RETURN(br->ReadBits(32,&time->time_scale));
  TRUE_OR_RETURN(br->ReadBool(&time->general_nal_hrd_params_present_flag));
  TRUE_OR_RETURN(br->ReadBool(&time->general_vcl_hrd_params_present_flag));
  if( time->general_nal_hrd_params_present_flag || time->general_vcl_hrd_params_present_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&time->general_same_pic_timing_in_all_ols_flag));
    TRUE_OR_RETURN(br->ReadBool(&time->general_du_hrd_params_present_flag));
    if( time->general_du_hrd_params_present_flag ){
      TRUE_OR_RETURN(br->ReadBits(8,&time->tick_divisor_minus2));
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



  if( sps->sps_long_term_ref_pics_flag && rplsIdx < sps->sps_num_ref_pic_lists[listIdx] && rpls->num_ref_entries[listIdx][rplsIdx] > 0 ){
    TRUE_OR_RETURN(br->ReadBool(&tmp_ltrp_in_header_flag));
    rpls->ltrp_in_header_flag[ listIdx ][ rplsIdx ].push_back(tmp_ltrp_in_header_flag);
  }
  
  for( int i = 0, j = 0; i < rpls->num_ref_entries[ listIdx ][ rplsIdx ]; i++) {
    if( sps->sps_inter_layer_prediction_enabled_flag ){
          TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
          rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx][i].push_back(tmp_inter_layer_ref_pic_flag);
    }
    if( !rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx][i] ) {
      if( sps->sps_long_term_ref_pics_flag ){
        TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
        rpls->st_ref_pic_flag[listIdx][rplsIdx][i].push_back(tmp_inter_layer_ref_pic_flag);
      }
      if( rpls->st_ref_pic_flag[listIdx][rplsIdx][i]) {
        TRUE_OR_RETURN(br->ReadUE(&tmp_st_ref_pic_flag));
        rpls->abs_delta_poc_st[listIdx][rplsIdx][i].push_back(tmp_st_ref_pic_flag);
        //compute AbsDeltaPocSt
        int abs_delta_poc_st_value = 0;
        if( ( sps->sps_weighted_pred_flag || sps->sps_weighted_bipred_flag ) && i != 0 )
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
      
      } else if( !rpls->ltrp_in_header_flag[listIdx][rplsIdx] ){
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

H266Parser::Result H266Parser::Ols_Timing_Hrd_parameters(int firstsublayer, int sps_max_sublayers_minus1,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266OlsTimingHrdParameters* olf){
LOG(INFO) << "Parsing H.266 Ols Timing Hrd parameters";
  //7.3.5.2 OLS timing and HRD parameters 
  bool tmp_fixed_pic_rate_general_flag = false;
  bool tmp_fixed_pic_rate_within_cvs_flag = false;;
  int tmp_elemental_duration_in_tc_minus1 = 0;
  int tmp_low_delay_hrd_flag = 0;
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

        if(sps.general_timing_hrd_parameters.general_nal_hrd_params_present_flag ){
          //todo make function for this 
          for( int j = 0; j <= sps.timing.hrd_cpb_cnt_minus1; j++ ) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);
            if( sps->timing.general_du_hrd_params_present_flag ) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_du_value_minus1));
              olf->cpb_size_du_value_minus1[i][j].push_back(tmp_cpb_size_du_value_minus1);
              
              TRUE_OR_RETURN(br->ReadUE(&tmp_bit_rate_du_value_minus1));
              olf->bit_rate_du_value_minus1[i][j].push_back(tmp_bit_rate_du_value_minus1);
            }
            TRUE_OR_RETURN(br->ReadBool(&tmp_cbr_flag));
            olf->cbr_flag[i][j].push_back(tmp_cbr_flag);
          }
        }
        if(sps->timing.general_vcl_hrd_params_present_flag){
          for( int j = 0; j <= sps->timing.hrd_cpb_cnt_minus1; j++ ) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);
            if( sps->timing.general_du_hrd_params_present_flag ) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_du_value_minus1));
              olf->cpb_size_du_value_minus1[i][j].push_back(tmp_cpb_size_du_value_minus1);
              
              TRUE_OR_RETURN(br->ReadUE(&tmp_bit_rate_du_value_minus1));
              olf->bit_rate_du_value_minus1[i][j].push_back(tmp_bit_rate_du_value_minus1);
            }
            TRUE_OR_RETURN(br->ReadBool(&tmp_cbr_flag));
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
    }

  }
  bool gci_alignment_zero_bit;  
  while( !br->byte_aligned()){
    TRUE_OR_RETURN(br->ReadBool(&gci_alignment_zero_bit));
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

H266Parser::Result H266Parser::ByteAlignment(H26xBitReader* br) {
  LOG(INFO) << "Performing byte alignment";
  TRUE_OR_RETURN(br->SkipBits(1));
  TRUE_OR_RETURN(br->SkipBits(br->NumBitsLeft() % 8));
  return kOk;
}

// Stub implementations for methods that need to be defined
H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu, 
                                               H266SliceHeader* slice_header,
                                               const H266PictureHeader* picture_header) {
  // Implementation would use picture_header context
  LOG(INFO) << "STUB Parsing H.266 Slice Header with Picture Header context";

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
    int temp_profile;
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




   




/*       need to clean thos old code below as                */

#if 0
    
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
#endif

  return kOk;
}
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