// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/codecs/h266_parser.h>
#include "packager/media/codecs/h26x_bit_reader.h"


#include <algorithm>
#include <cmath>
#include <numeric>  // Add this include for std::accumulate

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
struct GeneralTimingHrdParameters;
struct H266RefPicListEntry;
struct H266ReferencePicListStruct;
struct H266PredWeightTable;
struct H266PictureHeaderStructure;
struct H266SpsRangeExtension;
struct H266AccessUnitDelimiter;

struct H266Aps;
struct H266AlfData;
struct H266LmcsData;
struct H266Scalinglistdata;




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

H266Pps::H266Pps() {}
H266Pps::~H266Pps() {}

H266Sps::H266Sps() {}
H266Sps::~H266Sps() {}

H266Vps::H266Vps() {}
H266Vps::~H266Vps() {}

//H266Aps::H266Aps() {}
H266Aps::~H266Aps() {}

H266PictureHeader::H266PictureHeader() {}
H266PictureHeader::~H266PictureHeader() {}

H266SliceHeader::H266SliceHeader() {}
H266SliceHeader::~H266SliceHeader() {}

H266VuiParameters::H266VuiParameters() {}
H266VuiParameters::~H266VuiParameters() {}

H266ProfileTierLevel::H266ProfileTierLevel() {}
H266ProfileTierLevel::~H266ProfileTierLevel() {}

GeneralTimingHrdParameters::GeneralTimingHrdParameters() {}
GeneralTimingHrdParameters::~GeneralTimingHrdParameters() {}

H266RefPicListEntry::H266RefPicListEntry() {}
H266RefPicListEntry::~H266RefPicListEntry() {}

H266ReferencePicListStruct::H266ReferencePicListStruct() {}
H266ReferencePicListStruct::~H266ReferencePicListStruct() {}

H266ReferencePicList::H266ReferencePicList() {}
H266ReferencePicList::~H266ReferencePicList() {}

H266PredWeightTable::H266PredWeightTable() {}
H266PredWeightTable::~H266PredWeightTable() {}

H266PictureHeaderRbsp::H266PictureHeaderRbsp() {}
H266PictureHeaderRbsp::~H266PictureHeaderRbsp() {}

H266SpsRangeExtension::H266SpsRangeExtension() {}
H266SpsRangeExtension::~H266SpsRangeExtension() {}

H266DPB_Parameters::H266DPB_Parameters() {}
H266DPB_Parameters::~H266DPB_Parameters() {}

H266OlsTimingHrdParameters::H266OlsTimingHrdParameters() {}
H266OlsTimingHrdParameters::~H266OlsTimingHrdParameters() {}

H266AccessUnitDelimiter::H266AccessUnitDelimiter() {}
H266AccessUnitDelimiter::~H266AccessUnitDelimiter() {}

H266AlfData::H266AlfData() {};
H266AlfData::~H266AlfData() {};

H266LmcsData::H266LmcsData() {};
H266LmcsData::~H266LmcsData() {};

H266Scalinglistdata::H266Scalinglistdata() {};
H266Scalinglistdata::~H266Scalinglistdata() {};





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

H266Parser::Result H266Parser::ParseAccessUnitDelimeter_Rbsp(const Nalu& nalu, int *aud_id) {
  LOG(INFO) << "Parsing H.266 AUD Header NALU";

  //7.4.3.10 AU delimiter RBSP semantics
  *aud_id = -1;
  std::unique_ptr<H266Vps> vps(new H266Vps);

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  int temporal_id = nalu.nuh_temporal_id();

  bool tmp_aud_irap_or_gdr_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_aud_irap_or_gdr_flag));
  DLOG(INFO) << "## aud_irap_or_gdr_flag : " << ( tmp_aud_irap_or_gdr_flag ? "1" : "0");
  int aud_pic_type = 0;
  TRUE_OR_RETURN(br->ReadBits(3,&aud_pic_type));
  DLOG(INFO) << "## aud_pic_type : " << ( aud_pic_type ? "1" : "0");
  int aud_type = aud_pic_type;
  switch (aud_type)
  {
  case 0:
    DLOG(INFO) << "aud_type for Intra: " << aud_type;
    break;
  case 1:
    DLOG(INFO) << "aud_type for I or P slice : " << aud_type;
    break;
  case 2:
    DLOG(INFO) << "aud_type for B, P or I slice : " << aud_type;
    break;
  default:
    DLOG(INFO) << " incorrect aud_type ";
    break;
  }

  OK_OR_RETURN(rbsp_trailing_bits(br));

  if(temporal_id == 0){
    DLOG(INFO) << " We create a vps instance to stock TemporalId From aud"  << temporal_id;
    *aud_id = temporal_id;
    active_vpses_.emplace(*aud_id, std::move(vps));
  }
  return kOk;
};

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

  //const
  H266Pps* pps = nullptr;
  //const
  H266Sps* sps = nullptr;
  pps = GetPps(slice_header->phs->ph_pic_parameter_set_id);
  /*
  if (!pps) {
      LOG(ERROR) << "PPS " << slice_header->phs->ph_pic_parameter_set_id
                 << " from picture header not found";
      //return kInvalidStream;
  }
      */

 if (!HasPps(phs->ph_pic_parameter_set_id)) {
     DLOG(ERROR) << "PPS " << phs->ph_pic_parameter_set_id << " not found";
     DebugPrintAvailableSets();
     return kInvalidStream;
 }
 H266Pps* pps = GetPps(phs->ph_pic_parameter_set_id);
   if (!pps) {
     DLOG(ERROR) << "GetPps returned nullptr";
     return kInvalidStream;
   }
   if (!pps) {
     DLOG(WARNING) << "Trying first available PPS";
     pps = GetFirstPps();
     if (!pps) {
       DLOG(ERROR) << "No PPS available at all";
       return kInvalidStream;
     }
   }







/*
  sps = GetSps(pps->pps_seq_parameter_set_id);
  if (!sps) {
    LOG(ERROR) << "SPS " << pps->pps_seq_parameter_set_id
               << " referenced by PPS " << pps->pic_parameter_set_id << " not found";
    //return kInvalidStream;
  }


   */


 if (!HasSps(pps->pps_seq_parameter_set_id)) {
     DLOG(ERROR) << "SPS " << pps->pps_seq_parameter_set_id << " not found";
     DebugPrintAvailableSets();
     return kInvalidStream;
 }
 H266Sps* sps = GetSps(phs->ph_pic_parameter_set_id);
   if (!sps) {
     DLOG(ERROR) << "GetSps returned nullptr";
     return kInvalidStream;
   }
   if (!sps) {
     DLOG(WARNING) << "Trying first available SPS";
     sps = GetFirstSps();
     if (!sps) {
       DLOG(ERROR) << "No SPS available at all";
       return kInvalidStream;
     }
   }




  //const H266Pps* pps = GetPps(slice_header->pic_parameter_set_id);
  TRUE_OR_RETURN(pps);
  DLOG(INFO) << "Found PPS " << slice_header->phs->ph_pic_parameter_set_id
               << " from picture header in slice";

  //const H266Sps* sps = GetSps(pps->seq_parameter_set_id);
  TRUE_OR_RETURN(sps);
  DLOG(INFO) << "Successfully retrieved SPS " << pps->pps_seq_parameter_set_id;

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
    return -1;
}

void AddCtbsToSlice(std::vector<std::vector<int>>& CtbAddrInSlice,
                    std::vector<int>& NumCtusInSlice,
                    int PicWidthInCtbsY,
                    int sliceIdx,
                    int startX, int stopX, int startY, int stopY) {

    // Use push_back as in the original algorithm
    for (int ctbY = startY; ctbY < stopY; ctbY++) {
        for (int ctbX = startX; ctbX < stopX; ctbX++) {
            int ctbAddr = ctbY * PicWidthInCtbsY + ctbX;
            if (sliceIdx < 0 || static_cast<size_t>(sliceIdx) >= CtbAddrInSlice.size()) {
              LOG(ERROR) << "Invalid slice index in AddCtbsToSlice";
              //return kUnsupportedStream;
              // need analyse code return
            }
            CtbAddrInSlice[sliceIdx].push_back(ctbAddr);
            NumCtusInSlice[sliceIdx]++;
        }
    }
}
#if 0
std::vector<uint32_t> DeriveTileColumnBoundaries(int NumTileColumns,
                                           const std::vector<uint32_t>& ColWidthVal) {
    // Create boundary array with size NumTileColumns + 1
    std::vector<uint32_t> TileColBdVal(NumTileColumns + 1);

    // Initialize first boundary to 0
    TileColBdVal[0] = 0;

    // Calculate subsequent boundaries
    for (int i = 0; i < NumTileColumns; i++) {
        TileColBdVal[i + 1] = TileColBdVal[i] + ColWidthVal[i];
    }

    return TileColBdVal;
}
#else
std::vector<uint32_t> DeriveTileColumnBoundaries(int NumTileColumns,
                                           const std::vector<uint32_t>& ColWidthVal) {
    // VALIDATION 1: Check input parameters for validity
    if (NumTileColumns <= 0) {
        throw std::invalid_argument("NumTileColumns must be positive");
    }

    if (ColWidthVal.empty()) {
        throw std::invalid_argument("ColWidthVal cannot be empty");
    }

    // VALIDATION 2: Ensure ColWidthVal has enough elements
    // Use size_t for comparison to avoid signed/unsigned warning
    if (ColWidthVal.size() < static_cast<size_t>(NumTileColumns)) {
        throw std::invalid_argument("ColWidthVal size (" +
                                    std::to_string(ColWidthVal.size()) +
                                    ") is less than NumTileColumns (" +
                                    std::to_string(NumTileColumns) + ")");
    }

    // VALIDATION 3: Ensure no column width is zero or negative
    for (int i = 0; i < NumTileColumns; i++) {
        if (ColWidthVal[i] == 0) {
            throw std::invalid_argument("Column width at index " +
                                        std::to_string(i) + " is zero");
        }
    }

    // SAFE CREATION: Create boundary array with size NumTileColumns + 1
    std::vector<uint32_t> TileColBdVal(static_cast<size_t>(NumTileColumns) + 1);

    // Initialize first boundary to 0
    TileColBdVal[0] = 0;

    // Calculate subsequent boundaries with overflow protection
    uint32_t currentBoundary = 0;
    for (int i = 0; i < NumTileColumns; i++) {
        // VALIDATION 4: Check for arithmetic overflow
        if (UINT32_MAX - currentBoundary < ColWidthVal[i]) {
            throw std::overflow_error("Arithmetic overflow at column " +
                                      std::to_string(i));
        }

        currentBoundary += ColWidthVal[i];
        TileColBdVal[i + 1] = currentBoundary;
    }

    // OPTIONAL VALIDATION: Verify boundaries are strictly increasing
    for (size_t i = 1; i < TileColBdVal.size(); i++) {
        if (TileColBdVal[i] <= TileColBdVal[i - 1]) {
            throw std::runtime_error("Boundary values are not strictly increasing");
        }
    }

    // VALIDATION 5: Check that final boundary makes sense
    // (should be equal to total width in CTUs)
    // Note: This check might be optional depending on context
    uint32_t totalWidth = std::accumulate(ColWidthVal.begin(),
                                         ColWidthVal.begin() + NumTileColumns,
                                         0u);
    if (TileColBdVal[NumTileColumns] != totalWidth) {
        throw std::runtime_error("Final boundary doesn't match total width");
    }

    return TileColBdVal;
}

#endif

#if 0
std::vector<uint32_t> DeriveCtbToTileColRowIdx(int PicWidthInCtbsY,
                                      const std::vector<uint32_t>& TileColBdVal) {

    // create output tab
    std::vector<uint32_t> ctbToTileColIdx(PicWidthInCtbsY);
    size_t NumTileColumns = TileColBdVal.size() - 1;

    uint32_t tileX = 0;
    //int NumTileColumns = TileColBdVal.size() - 1;

    for (uint32_t ctbAddrX = 0; ctbAddrX <= static_cast<uint32_t> (PicWidthInCtbsY); ctbAddrX++) {
        // check  next tile
        if (tileX < static_cast<uint32_t>(NumTileColumns) && ctbAddrX == TileColBdVal[tileX + 1]) {
            tileX++;
        }
        //ctbToTileColIdx[ctbAddrX] = tileX;
        ctbToTileColIdx.push_back(tileX);
    }
    return ctbToTileColIdx;
}
#else
std::vector<uint32_t> DeriveCtbToTileColRowIdx(int PicWidthInCtbsY,
                                      const std::vector<uint32_t>& TileColBdVal) {
    // VALIDATION: Check input parameters
    if (PicWidthInCtbsY <= 0) {
        LOG(ERROR) << "Invalid PicWidthInCtbsY: " << PicWidthInCtbsY;
        return std::vector<uint32_t>();  // Return empty vector
    }
    
    if (TileColBdVal.empty()) {
        LOG(ERROR) << "TileColBdVal is empty";
        return std::vector<uint32_t>();  // Return empty vector
    }
    
    // Create output vector (but we'll use push_back instead of pre-allocation)
    std::vector<uint32_t> ctbToTileColIdx;
    ctbToTileColIdx.reserve(PicWidthInCtbsY + 1);  // Reserve space for efficiency
    
    size_t NumTileColumns = TileColBdVal.size() - 1;
    uint32_t tileX = 0;
    
    for (uint32_t ctbAddrX = 0; ctbAddrX <= static_cast<uint32_t>(PicWidthInCtbsY); ctbAddrX++) {
        // Check if we should move to next tile
        if (tileX < NumTileColumns && 
            static_cast<size_t>(tileX + 1) < TileColBdVal.size() && 
            ctbAddrX == TileColBdVal[tileX + 1]) {
            tileX++;
        }
        ctbToTileColIdx.push_back(tileX);
    }
    
    return ctbToTileColIdx;
}

#endif 

// Compute tiles coulumns
#if 0
std::vector<uint32_t> CalculateColWidthVal(const H266Pps& pps, int PicWidthInCtbsY) {
    std::vector<uint32_t> ColWidthVal;
    int remainingWidth = PicWidthInCtbsY;

    // Tiles explicites
    for (int i = 0; i <= pps.pps_num_exp_tile_columns_minus1; i++) {
        uint32_t width = static_cast<uint32_t>(pps.pps_tile_column_width_minus1[i] + 1);
        ColWidthVal.push_back(width);
        remainingWidth -= static_cast<int>(width);
    }

    // Tiles uniformes
    if (static_cast<int>(pps.pps_num_exp_tile_columns_minus1) < static_cast<int>(pps.NumTileColumns) - 1) {
        uint32_t uniformWidth = static_cast<uint32_t>(pps.pps_tile_column_width_minus1[pps.pps_num_exp_tile_columns_minus1] + 1);

        while (remainingWidth > 0 && ColWidthVal.size() < pps.NumTileColumns) {
            uint32_t width = std::min(static_cast<uint32_t>(remainingWidth), uniformWidth);
            ColWidthVal.push_back(width);
            remainingWidth -= static_cast<int>(width);
        }
    }

    return ColWidthVal;
}
#else

std::vector<uint32_t> CalculateColWidthVal(const H266Pps& pps, int PicWidthInCtbsY) {
    std::vector<uint32_t> ColWidthVal;

    int NumTileColumns = pps.num_tile_columns_minus1 + 1;
    int remainingWidth = PicWidthInCtbsY;

    // In VVC/H.266, pps_num_exp_tile_columns_minus1 indicates the number of
    // explicitly specified tile column widths MINUS 1
    // So the actual number of explicit widths is (pps_num_exp_tile_columns_minus1 + 1)
    int numExplicitWidths = pps.pps_num_exp_tile_columns_minus1 + 1;

    // Ensure numExplicitWidths doesn't exceed NumTileColumns
    numExplicitWidths = std::min(numExplicitWidths, NumTileColumns);

    size_t currentSize = pps.pps_tile_column_width_minus1.size();
    LOG(INFO) << "Calculating Column Width Values: NumTileColumns=" << NumTileColumns
              << ", numExplicitWidths=" << numExplicitWidths
              << ", pps_tile_column_width_minus1 size=" << currentSize;


    // Process explicit widths
    size_t widthsToProcess = std::min(static_cast<size_t>(numExplicitWidths), currentSize);

    //for (int i = 0; i < numExplicitWidths; i++) {
    for (size_t i = 0; i < widthsToProcess; i++) {
        // Check bounds before accessing
        LOG(INFO) << "Processing explicit tile column width index: " << i << "pps_tile_column_width_minus1 size: " << pps.pps_tile_column_width_minus1.size();
        if (static_cast<size_t>(i) >= pps.pps_tile_column_width_minus1.size()) {
            throw std::runtime_error("Invalid tile column width index");
        }

        uint32_t width = static_cast<uint32_t>(pps.pps_tile_column_width_minus1[i] + 1);
        ColWidthVal.push_back(width);
        remainingWidth -= static_cast<int>(width);
    }

    // If we have processed fewer columns than NumTileColumns, add uniform tiles
    if (numExplicitWidths < NumTileColumns) {
        // The uniform width is derived differently in VVC
        // If there are explicit widths, the uniform width should be based on
        // the remaining width divided by remaining columns
        int numRemainingColumns = NumTileColumns - numExplicitWidths;

        if (numRemainingColumns <= 0) {
            throw std::runtime_error("Invalid number of remaining columns");
        }

        // Check if there's enough remaining width
        if (remainingWidth <= 0) {
            throw std::runtime_error("Not enough remaining width for uniform tiles");
        }

        // Distribute remaining width among remaining columns
        // The VVC standard typically uses ceil division for the first few
        // and gives the remainder to the last column
        for (int i = 0; i < numRemainingColumns; i++) {
            // Calculate width using ceil division
            int width = (remainingWidth + (numRemainingColumns - i - 1)) / (numRemainingColumns - i);
            ColWidthVal.push_back(static_cast<uint32_t>(width));
            remainingWidth -= width;
        }
    }

    // Validate that we have exactly NumTileColumns
    if (static_cast<int>(ColWidthVal.size()) != NumTileColumns) {
        throw std::runtime_error("Generated incorrect number of tile columns");
    }

    // Validate that total width matches PicWidthInCtbsY
    int totalWidth = 0;
    for (uint32_t width : ColWidthVal) {
        totalWidth += static_cast<int>(width);
    }

    if (totalWidth != PicWidthInCtbsY) {
        throw std::runtime_error("Total column width doesn't match picture width");
    }

    return ColWidthVal;
}
#endif

bool ValidateSliceHeader(const H266SliceHeader* slice_header,
                        const H266Sps* sps,
                        const H266Pps* pps) {
    // Vérifier que CurrSubpicIdx est valide
    if (slice_header->CurrSubpicIdx < 0 ||
        slice_header->CurrSubpicIdx > sps->sps_num_subpics_minus1) {
        return false;
    }

    // Vérifier la cohérence des dimensions
    if (slice_header->PicWidthInCtbsY <= 0 ||
        slice_header->PicHeightInCtbsY <= 0) {
        return false;
    }

    return true;
}
H266Parser::Result H266Parser::ParsePictureHeaderRbsp(const Nalu& nalu, H266PictureHeaderRbsp *pictureheaderrbsp){

    LOG(INFO) << "Parsing H.266 Picture Header Rbsp NALU";

  // Parses whole element.
  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  pictureheaderrbsp->phs.emplace();
    //picture_header_structure( )
    //H266PictureHeaderStructure* phs
    TRUE_OR_RETURN(ParsePictureHeaderStructure(nalu, &pictureheaderrbsp->phs.value()));

  OK_OR_RETURN(rbsp_trailing_bits(br));
  return kOk;
}


H266Parser::Result H266Parser::ParseSliceHeader(const Nalu& nalu,
                                                H266SliceHeader* slice_header) {
  LOG(INFO) << "Parsing H.266 Slice Header NALU";

   //7.3.2.14 Slice layer RBSP syntax
  //std::unique_ptr<H266Sps> sps(new H266Sps);
  //std::unique_ptr<H266Pps> pps(new H266Pps);

  //need extract
/*   sh_slice_type
  sh_num_ref_idx_active_override_flag
  sh_num_ref_idx_active_minus1
  to calcultate  NumRefIdxActive[  equation 139 page 157
  Weight Predic  func
 */


   DCHECK(nalu.is_video_slice());
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
  *slice_header = H266SliceHeader{};//();
  #pragma GCC diagnostic pop


  if (!slice_header) {
      LOG(ERROR) << "slice_header is null";
  }

  // Parses whole element.
  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  bool tmp_sh_picture_header_in_slice_header_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_sh_picture_header_in_slice_header_flag));

  DLOG(INFO) << "## sh_picture_header_in_slice_header_flag : " << ( tmp_sh_picture_header_in_slice_header_flag ? "1" : "0");



  slice_header->sh_picture_header_in_slice_header_flag = tmp_sh_picture_header_in_slice_header_flag;
  if(slice_header->sh_picture_header_in_slice_header_flag){
    slice_header->phs.emplace();
    //picture_header_structure( )
    TRUE_OR_RETURN(ParsePictureHeaderStructure(nalu, &slice_header->phs.value()));
   }
   //const
   #if 0
   H266Pps* pps = GetPps(slice_header->phs->ph_pic_parameter_set_id);
   TRUE_OR_RETURN(pps);
   #else

    if (!HasPps(slice_header->phs->ph_pic_parameter_set_id)) {
     DLOG(ERROR) << "PPS " << slice_header->phs->ph_pic_parameter_set_id << " not found";
     DebugPrintAvailableSets();
     return kInvalidStream;
    }
    H266Pps* pps = GetPps(slice_header->phs->ph_pic_parameter_set_id);
    if (!pps) {
        DLOG(ERROR) << "GetPps returned nullptr";
        return kInvalidStream;
    }
    if (!pps) {
        DLOG(WARNING) << "Trying first available PPS";
        pps = GetFirstPps();
        if (!pps) {
          DLOG(ERROR) << "No PPS available at all";
          return kInvalidStream;
        }
    }

   #endif


#if 0
   //const
   H266Sps* sps = GetSps(pps->seq_parameter_set_id);
   TRUE_OR_RETURN(sps);
#else

  if (!HasSps(pps->pps_seq_parameter_set_id)) {
      DLOG(ERROR) << "SPS " << pps->pps_seq_parameter_set_id << " not found";
      DebugPrintAvailableSets();
      return kInvalidStream;
  }
  H266Sps* sps = GetSps(pps->seq_parameter_set_id);
    if (!sps) {
      DLOG(ERROR) << "GetSps returned nullptr";
      return kInvalidStream;
    }
    if (!sps) {
      DLOG(WARNING) << "Trying first available SPS";
      sps = GetFirstSps();
      if (!sps) {
        DLOG(ERROR) << "No SPS available at all";
        return kInvalidStream;
      }
    }
#endif

   /* if (!ValidateSliceHeader(slice_header, sps, pps)) {
    LOG(ERROR) << "Slice header validation failed";
    return kInvalidStream;
  } */





  if(sps->sps_subpic_info_present_flag ){
    int tmp_sh_subpic_id = 0;
    int len_sh_subpic_id = sps->sps_subpic_id_len_minus1 + 1;
    TRUE_OR_RETURN(br->ReadBits(len_sh_subpic_id,&tmp_sh_subpic_id));
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
  //pps->CtbSizeY = 5; //coredump

  slice_header->CurrSubpicIdx = 0;

  //int NumTilesInPic = NumTileColumns*NumTileRows;

  if(slice_header->sh_subpic_id){
        slice_header->CurrSubpicIdx = findSubpicIndex(slice_header->sh_subpic_id,slice_header->SubpicIdVal);
        if(slice_header->CurrSubpicIdx == -1) {
            LOG(ERROR) << "Subpic ID " << slice_header->sh_subpic_id << " not found in SubpicIdVal";
            return kInvalidStream;
        }
        if (slice_header->CurrSubpicIdx < 0 ||  slice_header->CurrSubpicIdx > sps->sps_num_subpics_minus1) {
           LOG(ERROR) << "CurrSubpicIdx out of range: " << slice_header->CurrSubpicIdx;
           return kInvalidStream;
        }
  }else{
    slice_header->CurrSubpicIdx = 0;
  }



/*
  The lists NumSlicesInSubpic[ i ], SubpicLevelSliceIdx[ j ], and SubpicIdxForSlice[ j ], specifying the number of slices
in the i-th subpicture, the subpicture-level slice index of the slice with picture-level slice index j, and the subpicture index
of the slice with picture-level slice index j, respectively, are derived as follows:
PAGE 32
 */
 //PicWidthInCtbsY eq 64 page 118
  int tmp_ctbSizeY = 0;
  tmp_ctbSizeY = std::max(pps->CtbSizeY,sps->CtbSizeY);

  // I ADD THIS FOR FOR SURE TO GET CtbSizeY , this value  can be evaluate in pps and also sps

 LOG(INFO) << " CtbSizeYs" << tmp_ctbSizeY;

 //if(pps->CtbSizeY != 0){
 if(tmp_ctbSizeY != 0){

  // slice_header->PicWidthInCtbsY = ceil( pps->pps_pic_width_in_luma_samples / pps->CtbSizeY );
  // slice_header->PicHeightInCtbsY = ceil( pps->pps_pic_height_in_luma_samples / pps->CtbSizeY );

   slice_header->PicWidthInCtbsY = ceil( pps->pps_pic_width_in_luma_samples / tmp_ctbSizeY );
   LOG(INFO) << " PicWidthInCtbsY " << slice_header->PicWidthInCtbsY;

   slice_header->PicHeightInCtbsY = ceil( pps->pps_pic_height_in_luma_samples / tmp_ctbSizeY );
    LOG(INFO) << " PicHeightInCtbsY " << slice_header->PicHeightInCtbsY;


 }else {
    LOG(ERROR) << "Invalid CtbSizeYs";
    return kInvalidStream;
 }

if (slice_header->PicWidthInCtbsY <= 0 || slice_header->PicHeightInCtbsY <= 0) {
    LOG(ERROR) << "Invalid picture dimensions in CTBs";
    return kInvalidStream;
}



 // need populate CtbAddrInSlice  eq 22 pgae 32 ...
 /****************************************************************************************/
//todo AddCtbsToSlice func;                        ok
//NumCtusInSlice[]                                 0k
//slice_header->subpicHeightLessThanOneTileFlag[]  ok
//slice_header->ctbToTileColIdx[]                  ok
//slice_header->ctbToTileRowIdx                    ok
//slice_header->SubpicHeightInTiles[]
//slice_header->SubpicWidthInTiles[]

//CtbAddrInSlice                                  0k


//slice_header->TileColBdVal[]   need ColWidthVal[  ok
//slice_header->TileRowBdVal[]  need RowHeightVal[ ok
/***************************need ColWidthVal to compute TileColBdVal ************************* */
//6.5.1 CTB raster scanning, tile scanning, and subpicture scanning processes
//ColWidthVal  equqtion 14 Page 28

/*
int local_NumTileColumns = 0;
int inc_i= 0;
int remainingWidthInCtbsY = slice_header->PicWidthInCtbsY;
for( int i = 0; i <= pps->pps_num_exp_tile_columns_minus1; i++ ) {
  int tmp_sum_width = pps->pps_tile_column_width_minus1[i] + 1;
  slice_header->ColWidthVal.push_back(tmp_sum_width);
  remainingWidthInCtbsY -= slice_header->ColWidthVal[i];
}
uint32_t uniformTileColWidth = pps->pps_tile_column_width_minus1[pps->pps_num_exp_tile_columns_minus1] + 1;
while( remainingWidthInCtbsY >= uniformTileColWidth && inc_i < slice_header->ColWidthVal.size() ) {
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
  LOG(ERROR) << "NumTileColumns mismatch";
  return kInvalidStream;
}

 */
LOG(INFO) << " Calculating ColWidthVal for Slice Header";
LOG(INFO) << " PicWidthInCtbsY: " << slice_header->PicWidthInCtbsY;


slice_header->ColWidthVal = CalculateColWidthVal(*pps, slice_header->PicWidthInCtbsY);

 /***************************************************************************/
 LOG(INFO) << " Deriving Tile Column Boundaries for Slice Header";
 //LOG(INFO) << " ColWidthVal " << slice_header->ColWidthVal;


slice_header->TileColBdVal = DeriveTileColumnBoundaries(NumTileColumns, slice_header->ColWidthVal);

if (slice_header->TileColBdVal.size() != NumTileColumns + 1) {
    LOG(ERROR) << "Failed to derive tile column boundaries";
    return kInvalidStream;
}



/*******************compute RowHeightVal[**********************************/
uint32_t remainingHeightInCtbsY = static_cast<uint32_t>(slice_header->PicHeightInCtbsY);
uint32_t inc_y = 0;
uint32_t local_NumTileRows = 0;
for( int j = 0; j <= pps->pps_num_exp_tile_rows_minus1; j++ ) {
  int tmp_sum_height = pps->pps_tile_row_height_minus1[j] + 1;
  slice_header->RowHeightVal.push_back(tmp_sum_height);
  remainingHeightInCtbsY -= slice_header->RowHeightVal[j];
}
uint32_t uniformTileRowHeight = pps->pps_tile_row_height_minus1[ pps->pps_num_exp_tile_rows_minus1 ] + 1;

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
LOG(INFO) << " Deriving Tile Row Boundaries for Slice Header";

slice_header->TileRowBdVal = DeriveTileColumnBoundaries(NumTileRows, slice_header->RowHeightVal);

/******************* CtbToTileRowBd[ eq 19 page 29 **********************************/

//slice_header->CtbToTileRowBd =
LOG(INFO) << " Deriving CtbToTileColBd for Slice Header";

auto tempCol = DeriveCtbToTileColRowIdx(slice_header->PicWidthInCtbsY, slice_header->TileColBdVal);

slice_header->CtbToTileColBd.assign(tempCol.begin(), tempCol.end());

/******************************************ctbToTileColIdx eq 18 page 29 *******************************/

//slice_header->CtbToTileRowBd
LOG(INFO) << " Deriving CtbToTileRowBd for Slice Header";
auto tempRow = DeriveCtbToTileColRowIdx(slice_header->PicHeightInCtbsY, slice_header->TileRowBdVal);
slice_header->CtbToTileRowBd.assign(tempRow.begin(), tempRow.end());

/**********************ctbToTileColIdx eq 18 page 29 *******************************/
LOG(INFO) << " Deriving ctbToTileColIdx and ctbToTileRowIdx for Slice Header";

//slice_header->ctbToTileColIdx = DeriveCtbToTileColRowIdx(slice_header->PicWidthInCtbsY, slice_header->TileColBdVal);
auto tempColIdx =  DeriveCtbToTileColRowIdx(slice_header->PicWidthInCtbsY, slice_header->TileColBdVal);
slice_header->ctbToTileColIdx.assign( tempColIdx.begin(), tempColIdx.end());


LOG(INFO) << " Deriving ctbToTileRowIdx for Slice Header";

//slice_header->ctbToTileRowIdx = DeriveCtbToTileColRowIdx(slice_header->PicWidthInCtbsY, slice_header->TileRowBdVal);
auto tempRowIdx = DeriveCtbToTileColRowIdx(slice_header->PicHeightInCtbsY, slice_header->TileRowBdVal);
slice_header->ctbToTileRowIdx.assign(tempRowIdx.begin(), tempRowIdx.end());

slice_header->SubpicWidthInTiles.resize(sps->sps_num_subpics_minus1 + 1);
slice_header->SubpicHeightInTiles.resize(sps->sps_num_subpics_minus1 + 1);
slice_header->subpicHeightLessThanOneTileFlag.resize(sps->sps_num_subpics_minus1 + 1);

/***********************************************************************************************/

if (sps->sps_subpic_ctu_top_left_x.size() <= static_cast<size_t>(sps->sps_num_subpics_minus1) ||
    sps->sps_subpic_width_minus1.size() <= static_cast<size_t>(sps->sps_num_subpics_minus1) ||
    sps->sps_subpic_ctu_top_left_y.size() <= static_cast<size_t>(sps->sps_num_subpics_minus1) ||
    sps->sps_subpic_height_minus1.size() <= static_cast<size_t>(sps->sps_num_subpics_minus1)) {
    
    LOG(ERROR) << "Subpicture vectors not properly initialized. Sizes: "
               << "ctu_top_left_x=" << sps->sps_subpic_ctu_top_left_x.size()
               << ", width_minus1=" << sps->sps_subpic_width_minus1.size()
               << ", ctu_top_left_y=" << sps->sps_subpic_ctu_top_left_y.size()
               << ", height_minus1=" << sps->sps_subpic_height_minus1.size()
               << ", expected at least " << sps->sps_num_subpics_minus1 + 1;
    return kInvalidStream;
}

if (sps->subpic_ctu_top_left_x_.empty() || sps->subpic_ctu_top_left_y_.empty() ||
    sps->subpic_width_minus1_.empty() || sps->subpic_height_minus1_.empty()) {
  LOG(ERROR) << "Subpicture vectors not initialized";
  return kInvalidStream;  // Or appropriate error code
}


/***************** subpicHeightLessThanOneTileFlag  equqtion 20 page 30 ************************************/
for( int i = 0; i <= sps->sps_num_subpics_minus1; i++ ) {


  int leftX = sps->sps_subpic_ctu_top_left_x[i];
  int rightX = leftX + sps->sps_subpic_width_minus1[i];


  slice_header->SubpicWidthInTiles[i] = slice_header->ctbToTileColIdx[ rightX ] + 1 - slice_header->ctbToTileColIdx[ leftX ];
  int topY = sps->sps_subpic_ctu_top_left_y[i];
  int bottomY = topY + sps->sps_subpic_height_minus1[i];
  slice_header->SubpicHeightInTiles[i] = slice_header->ctbToTileRowIdx[bottomY] + 1 - slice_header->ctbToTileRowIdx[ topY ];

  if( slice_header->SubpicHeightInTiles[ i ] == 1 &&
      static_cast<uint32_t>(sps->sps_subpic_height_minus1[i] + 1) < slice_header->RowHeightVal[ slice_header->ctbToTileRowIdx[ topY ] ] ){
        slice_header->subpicHeightLessThanOneTileFlag[ i ] = true;
  }else {
    slice_header->subpicHeightLessThanOneTileFlag[ i ] = false;
  }
}
/************************************************************************************************************/


/************************************************************************************************************/
slice_header->NumCtusInSlice.clear();
slice_header->CtbAddrInSlice.resize(pps->pps_num_slices_in_pic_minus1 + 1);
for (int i = 0; i <= pps->pps_num_slices_in_pic_minus1; i++) {
    slice_header->CtbAddrInSlice[i].clear();
}

slice_header->NumSlicesInSubpic.resize(sps->sps_num_subpics_minus1 + 1, 0);



 if( pps->pps_single_slice_per_subpic_flag ) {
    if(!sps->sps_subpic_info_present_flag){
      for( uint32_t j = 0; j < NumTileRows; j++ ){
        for( uint32_t i = 0; i < NumTileColumns; i++ ){
          //AddCtbsToSlice( 0, TileColBdVal[ i ], TileColBdVal[ i + 1 ], TileRowBdVal[ j ],TileRowBdVal[ j + 1 ] );
          LOG(INFO) << " Adding CTBs to Slice for Tile Column " << i << " and Tile Row " << j;
          AddCtbsToSlice(slice_header->CtbAddrInSlice,
                                  slice_header->NumCtusInSlice,
                                  slice_header->PicWidthInCtbsY,
                                  0, slice_header->TileColBdVal[i], slice_header->TileColBdVal[i+1], slice_header->TileRowBdVal[ j ],slice_header->TileRowBdVal[j+1]);
        }
      }
    } else{
      for( int i = 0; i <= sps->sps_num_subpics_minus1; i++ ) {
        //NumCtusInSlice[ i ] = 0
        if( slice_header->subpicHeightLessThanOneTileFlag[ i ] ){ /* The slice consists of a set of CTU rows in a tile. */
          LOG(INFO) << " Adding CTBs to Slice for Subpicture " << i << " with height less than one tile";
          AddCtbsToSlice(slice_header->CtbAddrInSlice,
                                  slice_header->NumCtusInSlice,
                                  slice_header->PicWidthInCtbsY,
                                  i,
                                  sps->sps_subpic_ctu_top_left_x[i] ,
            sps->sps_subpic_ctu_top_left_x[ i ] + sps->sps_subpic_width_minus1[ i ] + 1,
            sps->sps_subpic_ctu_top_left_y[ i ],
            (sps->sps_subpic_ctu_top_left_y[i] + sps->sps_subpic_height_minus1[ i ] + 1));
        } else { /* The slice consists of a number of complete tiles covering a rectangular region. */
          int tileX = slice_header->ctbToTileColIdx[ sps->sps_subpic_ctu_top_left_x[i] ];
          int tileY = slice_header->ctbToTileRowIdx[ sps->sps_subpic_ctu_top_left_y[ i ] ];
          for( int j = 0; j < slice_header->SubpicHeightInTiles[ i ]; j++ ){
            for( int k = 0; k < slice_header->SubpicWidthInTiles[ i ]; k++ ){
              AddCtbsToSlice(slice_header->CtbAddrInSlice,
                                  slice_header->NumCtusInSlice,
                                  slice_header->PicWidthInCtbsY, i, slice_header->TileColBdVal[ tileX + k ], slice_header->TileColBdVal[ tileX + k + 1 ], slice_header->TileRowBdVal[ tileY + j ], slice_header->TileRowBdVal[ tileY + j + 1 ] );
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

   
  }

}
 /****************************************************************************************/

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
/**********************NumExtraShBits   eq 42 page 107   need funct later */
slice_header->NumExtraShBits = 0;
for( int i = 0; i < ( sps->sps_num_extra_sh_bytes * 8 ); i++ ){
   if( sps->sps_extra_sh_bit_present_flag[ i ] ){
    slice_header->NumExtraShBits += 1;
   }
}
/********************************************** */





   if( (pps->pps_rect_slice_flag && slice_header->NumSlicesInSubpic[slice_header->CurrSubpicIdx ] > 1 ) || ( !pps->pps_rect_slice_flag && pps->NumTilesInPic > 1 ) ){
   int len_sh_slice_address = ceil(log2(pps->NumTilesInPic));
   int tmp_slice_address = 0;
   TRUE_OR_RETURN(br->ReadBits(len_sh_slice_address, &tmp_slice_address));
   slice_header->sh_slice_address = tmp_slice_address;
   LOG(INFO) << " ## sh_slice_address : " << tmp_slice_address;
  }

  for( int i = 0; i < slice_header->NumExtraShBits; i++ ){
    bool tmp_sh_extra_bits = false;
    TRUE_OR_RETURN(br->ReadBool( &tmp_sh_extra_bits));
    slice_header->sh_extra_bits.push_back(tmp_sh_extra_bits);
    LOG(INFO) << " ## sh_extra_bits[ " << i << " ] : " << ( tmp_sh_extra_bits ? "1" : "0");
  }

  if( !pps->pps_rect_slice_flag && pps->NumTilesInPic - slice_header->sh_slice_address > 1 ){
    int tmp_sh_num_tiles_in_slice_minus1 = 0;
    TRUE_OR_RETURN(br->ReadUE(&tmp_sh_num_tiles_in_slice_minus1));
    slice_header->sh_num_tiles_in_slice_minus1 = tmp_sh_num_tiles_in_slice_minus1;
    LOG(INFO) << " ## sh_num_tiles_in_slice_minus1 : " << tmp_sh_num_tiles_in_slice_minus1;
  }

  if( slice_header->phs->ph_inter_slice_allowed_flag ){
    int tmp_sh_slice_type = 0;
    TRUE_OR_RETURN(br->ReadUE(&tmp_sh_slice_type));
    slice_header->sh_slice_type = tmp_sh_slice_type;
    DLOG(INFO) << "## sh_slice_type : " << tmp_sh_slice_type;

  }

  if( nalu.type() == Nalu::H266_IDR_W_RADL || nalu.type() == Nalu::H266_IDR_N_LP || nalu.type() == Nalu::H266_CRA_NUT || nalu.type() == Nalu::H266_GDR_NUT ){
    bool tmp_sh_no_output_of_prior_pics_flag;
    TRUE_OR_RETURN(br->ReadBool( &tmp_sh_no_output_of_prior_pics_flag));
    slice_header->sh_no_output_of_prior_pics_flag = tmp_sh_no_output_of_prior_pics_flag;
    DLOG(INFO) << "## sh_no_output_of_prior_pics_flag : " << ( tmp_sh_no_output_of_prior_pics_flag ? "1" : "0");
  }

  if( sps->sps_alf_enabled_flag && !pps->pps_alf_info_in_ph_flag ) {
    bool tmp_sh_alf_enabled_flag = false;
    TRUE_OR_RETURN(br->ReadBool( &tmp_sh_alf_enabled_flag));
    slice_header->sh_alf_enabled_flag = tmp_sh_alf_enabled_flag;
    DLOG(INFO) << "## sh_alf_enabled_flag : " << ( tmp_sh_alf_enabled_flag ? "1" : "0");

    if(slice_header->sh_alf_enabled_flag){
      int tmp_sh_num_alf_aps_ids_luma = 0;
      TRUE_OR_RETURN(br->ReadBits(3, &tmp_sh_num_alf_aps_ids_luma));
      slice_header->sh_num_alf_aps_ids_luma = tmp_sh_num_alf_aps_ids_luma;
      LOG(INFO) << " ## sh_num_alf_aps_ids_luma : " << tmp_sh_num_alf_aps_ids_luma;

      for( int i = 0; i < slice_header->sh_num_alf_aps_ids_luma; i++ ){
        int tmp_sh_alf_aps_id_luma = 0;
        TRUE_OR_RETURN(br->ReadBits(3, &tmp_sh_alf_aps_id_luma));
        slice_header->sh_alf_aps_id_luma.push_back(tmp_sh_alf_aps_id_luma);
        LOG(INFO) << " ## sh_alf_aps_id_luma[ " << i << " ] : " << tmp_sh_alf_aps_id_luma;
      }

      if( sps->sps_chroma_format_idc != 0 ) {
        bool tmp_sh_alf_cb_enabled_flag = false;
        bool tmp_sh_alf_cr_enabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool( &tmp_sh_alf_cb_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool( &tmp_sh_alf_cr_enabled_flag));
        slice_header->sh_alf_cb_enabled_flag = tmp_sh_alf_cb_enabled_flag;
        LOG(INFO) << "# sh_alf_cb_enabled_flag : " << ( tmp_sh_alf_cb_enabled_flag ? "1" : "0");
        slice_header->sh_alf_cr_enabled_flag = tmp_sh_alf_cr_enabled_flag;
        LOG(INFO) << "# sh_alf_cr_enabled_flag : " << ( tmp_sh_alf_cr_enabled_flag ? "1" : "0");
      }

      if( slice_header->sh_alf_cb_enabled_flag || slice_header->sh_alf_cr_enabled_flag ){
        int tmp_sh_alf_aps_id_chroma = 0;
        TRUE_OR_RETURN(br->ReadBits(3, &tmp_sh_alf_aps_id_chroma));
        slice_header->sh_alf_aps_id_chroma = tmp_sh_alf_aps_id_chroma;
        LOG(INFO) << " ## sh_alf_aps_id_chroma : " << tmp_sh_alf_aps_id_chroma;
      }

      if( sps->sps_ccalf_enabled_flag ) {
        bool tmp_sh_alf_cc_cb_enabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool( &tmp_sh_alf_cc_cb_enabled_flag));
        slice_header->sh_alf_cc_cb_enabled_flag = tmp_sh_alf_cc_cb_enabled_flag;
        LOG(INFO) << "# sh_alf_cc_cb_enabled_flag : " << ( tmp_sh_alf_cc_cb_enabled_flag ? "1" : "0");

        if(slice_header->sh_alf_cc_cb_enabled_flag){
          int tmp_sh_alf_cc_cb_aps_id = 0;
          TRUE_OR_RETURN(br->ReadBits(3, &tmp_sh_alf_cc_cb_aps_id));
          slice_header->sh_alf_cc_cb_aps_id = tmp_sh_alf_cc_cb_aps_id;
          LOG(INFO) << " # sh_alf_cc_cb_aps_id : " << tmp_sh_alf_cc_cb_aps_id;
        }
        bool tmp_sh_alf_cc_cr_enabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool( &tmp_sh_alf_cc_cr_enabled_flag));
        slice_header->sh_alf_cc_cr_enabled_flag = tmp_sh_alf_cc_cr_enabled_flag;
        LOG(INFO) << "# sh_alf_cc_cr_enabled_flag : " << ( tmp_sh_alf_cc_cr_enabled_flag ? "1" : "0");
        if(slice_header->sh_alf_cc_cr_enabled_flag){
          int tmp_sh_alf_cc_cr_aps_id = 0;
          TRUE_OR_RETURN(br->ReadBits(3, &tmp_sh_alf_cc_cr_aps_id));
          slice_header->sh_alf_cc_cr_aps_id = tmp_sh_alf_cc_cr_aps_id;
          LOG(INFO) << " # sh_alf_cc_cr_aps_id : " << tmp_sh_alf_cc_cr_aps_id;
        }
      }
    }
  }

  if( slice_header->phs->ph_lmcs_enabled_flag && !slice_header->sh_picture_header_in_slice_header_flag ){
    bool tmp_sh_lmcs_used_flag = false;
    TRUE_OR_RETURN(br->ReadBool( &tmp_sh_lmcs_used_flag));
    slice_header->sh_lmcs_used_flag = tmp_sh_lmcs_used_flag;
    LOG(INFO) << " # sh_lmcs_used_flag : " << ( tmp_sh_lmcs_used_flag ? "1" : "0");
  }

  if( slice_header->phs->ph_explicit_scaling_list_enabled_flag && !slice_header->sh_picture_header_in_slice_header_flag ){
     bool tmp_sh_explicit_scaling_list_used_flag = false;
    TRUE_OR_RETURN(br->ReadBool( &tmp_sh_explicit_scaling_list_used_flag));
     slice_header->sh_explicit_scaling_list_used_flag = tmp_sh_explicit_scaling_list_used_flag;
     LOG(INFO) << " # sh_explicit_scaling_list_used_flag : " << ( tmp_sh_explicit_scaling_list_used_flag ? "1" : "0");
  }

  if( !pps->pps_rpl_info_in_ph_flag && ( ( nalu.type() != Nalu::H266_IDR_W_RADL && nalu.type() != Nalu::H266_IDR_N_LP ) || sps->sps_idr_rpl_present_flag ) ){
    slice_header->rpl.emplace();

    //ref_pic_lists( )
    //Ref_Pic_List(*sps, *pps, br, &slice_header->rpl.value());
    if (slice_header->rpl.has_value()) {
          Ref_Pic_List(*sps, *pps, br, &slice_header->rpl.value());
    }


  }
  if( ( slice_header->sh_slice_type != kVvcISlice &&
    slice_header->rpl->num_ref_entries[ 0 ][ slice_header->rpl->RplsIdx[ 0 ] ] > 1 ) ||
     ( slice_header->sh_slice_type == kVvcBSlice &&
      slice_header->rpl->num_ref_entries[ 1 ][ slice_header->rpl->RplsIdx[ 1 ] ] > 1 ) ) {

        bool tmp_sh_num_ref_idx_active_override_flag = false;
        TRUE_OR_RETURN(br->ReadBool( &tmp_sh_num_ref_idx_active_override_flag));
        slice_header->sh_num_ref_idx_active_override_flag = tmp_sh_num_ref_idx_active_override_flag;
        LOG(INFO) << " # sh_num_ref_idx_active_override_flag : " << ( tmp_sh_num_ref_idx_active_override_flag ? "1" : "0");

        if(slice_header->sh_num_ref_idx_active_override_flag ){
          int num_ref_idx_active = slice_header->slice_type == kVvcBSlice ? 2: 1;
          int tmp_sh_num_ref_idx_active_minus1;
          for(int  i = 0; i < num_ref_idx_active;i++ ){
            if( slice_header->rpl->num_ref_entries[ i ][ slice_header->rpl->RplsIdx[ i ] ] > 1 ){
                TRUE_OR_RETURN(br->ReadUE( &tmp_sh_num_ref_idx_active_minus1));
                slice_header->sh_num_ref_idx_active_minus1.push_back(tmp_sh_num_ref_idx_active_minus1);
                LOG(INFO) << " # sh_num_ref_idx_active_minus1[ " << i << " ] : " << tmp_sh_num_ref_idx_active_minus1;
            }
          }
        }
      }
  // need evalulate NumRefIdxActive
  /********************************************************************************** */
  for( int i = 0; i < 2; i++ ) {
    if( slice_header->sh_slice_type == kVvcBSlice || ( slice_header->sh_slice_type == kVvcPSlice && i == 0 ) ) {
        if( slice_header->sh_num_ref_idx_active_override_flag ){
            slice_header->NumRefIdxActive[ i ] = slice_header->sh_num_ref_idx_active_minus1[i] + 1;
        } else {
          if( slice_header->rpl->num_ref_entries[ i ][ slice_header->rpl->RplsIdx[ i ] ] >= pps->pps_num_ref_idx_default_active_minus1[ i ] + 1 ){
                 slice_header->NumRefIdxActive[i] = pps->pps_num_ref_idx_default_active_minus1[i] + 1;
          } else {
                 slice_header->NumRefIdxActive[ i ] = slice_header->rpl->num_ref_entries[ i ][ slice_header->rpl->RplsIdx[ i ] ];
          }
        }
      }else {
        /* sh_slice_type = = I | | ( sh_slice_type = = P && i = = 1 ) */
           slice_header->NumRefIdxActive[ i ] = 0;

      }
    // to inject this value  in PredWeightTable func
    slice_header->rpl->reference_pic_list->NumRefIdxActive[i] = slice_header->NumRefIdxActive[i];

  }




  /********************************************************************************** */






  if( slice_header->sh_slice_type != kVvcISlice) {
      if( pps->pps_cabac_init_present_flag ){
        bool tmp_sh_cabac_init_flag = false;
        TRUE_OR_RETURN(br->ReadBool( &tmp_sh_cabac_init_flag));
        slice_header->sh_cabac_init_flag = tmp_sh_cabac_init_flag;
        LOG(INFO) << " # sh_cabac_init_flag : " << ( tmp_sh_cabac_init_flag ? "1" : "0");
      }

      if( slice_header->phs->ph_temporal_mvp_enabled_flag && !pps->pps_rpl_info_in_ph_flag ) {

        if( slice_header->sh_slice_type == kVvcBSlice ){
          bool tmp_sh_collocated_from_l0_flag = false;
          TRUE_OR_RETURN(br->ReadBool( &tmp_sh_collocated_from_l0_flag));
          slice_header->sh_collocated_from_l0_flag = tmp_sh_collocated_from_l0_flag;
          LOG(INFO) << " # sh_collocated_from_l0_flag : " << ( tmp_sh_collocated_from_l0_flag ? "1" : "0");
        }

        if( ( slice_header->sh_collocated_from_l0_flag && slice_header->NumRefIdxActive[ 0 ] > 1 ) ||
          ( ! slice_header->sh_collocated_from_l0_flag && slice_header->NumRefIdxActive[ 1 ] > 1 ) ){
            int tmp_sh_collocated_ref_idx = 0;
            TRUE_OR_RETURN(br->ReadUE( &tmp_sh_collocated_ref_idx));
            slice_header->sh_collocated_ref_idx = tmp_sh_collocated_ref_idx;
            LOG(INFO) << " # sh_collocated_ref_idx : " << tmp_sh_collocated_ref_idx;
          }

      }

      if( !pps->pps_wp_info_in_ph_flag && ( ( pps->pps_weighted_pred_flag && slice_header->sh_slice_type == kVvcPSlice ) || ( pps->pps_weighted_bipred_flag && slice_header->sh_slice_type == kVvcBSlice ) ) ) {
        //pred_weight_table( )
        slice_header->pwt.emplace();
        TRUE_OR_RETURN(PredWeightTable(*sps, *pps, br, &slice_header->rpl.value(), &slice_header->pwt.value(),slice_header->NumRefIdxActive[0]));


      }
  }


  if( !pps->pps_qp_delta_info_in_ph_flag ){
    int tmp_sh_qp_delta = 0;
    TRUE_OR_RETURN(br->ReadSE( &tmp_sh_qp_delta));
    slice_header->sh_qp_delta = tmp_sh_qp_delta;
    LOG(INFO) << " # sh_qp_delta : " << tmp_sh_qp_delta;
  }

  if( pps->pps_slice_chroma_qp_offsets_present_flag ) {
      int tmp_sh_cb_qp_offset = 0;
      int tmp_sh_cr_qp_offset = 0;
      TRUE_OR_RETURN(br->ReadSE(&tmp_sh_cb_qp_offset));
      LOG(INFO) << " # sh_cb_qp_offset : " << tmp_sh_cb_qp_offset;
      TRUE_OR_RETURN(br->ReadSE(&tmp_sh_cr_qp_offset));
      LOG(INFO) << " # sh_cr_qp_offset : " << tmp_sh_cr_qp_offset;
      slice_header->sh_cb_qp_offset = tmp_sh_cb_qp_offset;
      slice_header->sh_cr_qp_offset = tmp_sh_cr_qp_offset;

      if( sps->sps_joint_cbcr_enabled_flag ){
        int tmp_sh_joint_cbcr_qp_offset = 0;
        TRUE_OR_RETURN(br->ReadSE(&tmp_sh_joint_cbcr_qp_offset));
        slice_header->sh_joint_cbcr_qp_offset = tmp_sh_joint_cbcr_qp_offset;
        LOG(INFO) << " # sh_joint_cbcr_qp_offset : " << tmp_sh_joint_cbcr_qp_offset;
      }
  }

    if( pps->pps_cu_chroma_qp_offset_list_enabled_flag ){
      bool tmp_sh_cu_chroma_qp_offset_enabled_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_sh_cu_chroma_qp_offset_enabled_flag));
      slice_header->sh_cu_chroma_qp_offset_enabled_flag = tmp_sh_cu_chroma_qp_offset_enabled_flag;
      LOG(INFO) << " # sh_cu_chroma_qp_offset_enabled_flag : " << ( tmp_sh_cu_chroma_qp_offset_enabled_flag ? "1" : "0");
    }

    if( sps->sps_sao_enabled_flag && !pps->pps_sao_info_in_ph_flag ) {
      bool tmp_sh_sao_luma_used_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_sh_sao_luma_used_flag));
      slice_header->sh_sao_luma_used_flag = tmp_sh_sao_luma_used_flag;
      LOG(INFO) << " # sh_sao_luma_used_flag : " << ( tmp_sh_sao_luma_used_flag ? "1" : "0");

      if( sps->sps_chroma_format_idc != 0 ){
        bool tmp_sh_sao_chroma_used_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_sh_sao_chroma_used_flag));
        slice_header->sh_sao_chroma_used_flag = tmp_sh_sao_chroma_used_flag;
        LOG(INFO) << " # sh_sao_chroma_used_flag : " << ( tmp_sh_sao_chroma_used_flag ? "1" : "0");
      }

    }

    if( pps->pps_deblocking_filter_override_enabled_flag && !pps->pps_dbf_info_in_ph_flag ){
      bool tmp_sh_deblocking_params_present_flag = 0;
      TRUE_OR_RETURN(br->ReadBool(&tmp_sh_deblocking_params_present_flag));
      slice_header->sh_deblocking_params_present_flag = tmp_sh_deblocking_params_present_flag;
      LOG(INFO) << " # sh_deblocking_params_present_flag : " << ( tmp_sh_deblocking_params_present_flag ? "1" : "0");
    }

      if( slice_header->sh_deblocking_params_present_flag ) {

        if( !pps->pps_deblocking_filter_disabled_flag ){
          bool tmp_sh_deblocking_filter_disabled_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_sh_deblocking_filter_disabled_flag));
          slice_header->sh_deblocking_filter_disabled_flag = tmp_sh_deblocking_filter_disabled_flag;
          LOG(INFO) << " # sh_deblocking_filter_disabled_flag : " << ( tmp_sh_deblocking_filter_disabled_flag ? "1" : "0");
        }

        if( !slice_header->sh_deblocking_filter_disabled_flag ) {
          int tmp_sh_luma_beta_offset_div2 = 0;
          int tmp_sh_luma_tc_offset_div2 = 0;
          TRUE_OR_RETURN(br->ReadSE(&tmp_sh_luma_beta_offset_div2));
          LOG(INFO) << " # sh_luma_beta_offset_div2 : " << tmp_sh_luma_beta_offset_div2;
          TRUE_OR_RETURN(br->ReadSE(&tmp_sh_luma_tc_offset_div2));
          LOG(INFO) << " # sh_luma_tc_offset_div2 : " << tmp_sh_luma_tc_offset_div2;

          slice_header->sh_luma_beta_offset_div2 = tmp_sh_luma_beta_offset_div2;
          slice_header->sh_luma_tc_offset_div2 = tmp_sh_luma_tc_offset_div2;
          if( pps->pps_chroma_tool_offsets_present_flag ) {
            int tmp_sh_cb_beta_offset_div2;
            int tmp_sh_cb_tc_offset_div2;
            int tmp_sh_cr_beta_offset_div2;
            int tmp_sh_cr_tc_offset_div2;
            TRUE_OR_RETURN(br->ReadSE(&tmp_sh_cb_beta_offset_div2));
            LOG(INFO) << " # sh_cb_beta_offset_div2 : " << tmp_sh_cb_beta_offset_div2;
            TRUE_OR_RETURN(br->ReadSE(&tmp_sh_cb_tc_offset_div2));
            LOG(INFO) << " # sh_cb_tc_offset_div2 : " << tmp_sh_cb_tc_offset_div2;
            TRUE_OR_RETURN(br->ReadSE(&tmp_sh_cr_beta_offset_div2));
            LOG(INFO) << " # sh_cr_beta_offset_div2 : " << tmp_sh_cr_beta_offset_div2;
            TRUE_OR_RETURN(br->ReadSE(&tmp_sh_cr_tc_offset_div2));
            LOG(INFO) << " # sh_cr_tc_offset_div2 : " << tmp_sh_cr_tc_offset_div2;

            slice_header->sh_cb_beta_offset_div2 = tmp_sh_cb_beta_offset_div2;
            slice_header->sh_cb_tc_offset_div2 = tmp_sh_cb_tc_offset_div2;
            slice_header->sh_cr_beta_offset_div2 = tmp_sh_cr_beta_offset_div2;
            slice_header->sh_cr_tc_offset_div2 = tmp_sh_cr_tc_offset_div2;
          }

        }
      }

      if( sps->sps_dep_quant_enabled_flag ){
        bool tmp_sh_dep_quant_used_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_sh_dep_quant_used_flag));
        slice_header->sh_dep_quant_used_flag = tmp_sh_dep_quant_used_flag;
        LOG(INFO) << " # sh_dep_quant_used_flag : " << ( tmp_sh_dep_quant_used_flag ? "1" : "0");
      }

      if( sps->sps_sign_data_hiding_enabled_flag && !slice_header->sh_dep_quant_used_flag ){
         bool tmp_sh_sign_data_hiding_used_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_sh_sign_data_hiding_used_flag));
         slice_header->sh_sign_data_hiding_used_flag = tmp_sh_sign_data_hiding_used_flag;
         LOG(INFO) << " # sh_sign_data_hiding_used_flag : " << ( tmp_sh_sign_data_hiding_used_flag ? "1" : "0");
      }

      if( sps->sps_transform_skip_enabled_flag && !slice_header->sh_dep_quant_used_flag && !slice_header->sh_sign_data_hiding_used_flag ){
        bool tmp_sh_ts_residual_coding_disabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_sh_ts_residual_coding_disabled_flag));
        slice_header->sh_ts_residual_coding_disabled_flag = tmp_sh_ts_residual_coding_disabled_flag;
        LOG(INFO) << " # sh_ts_residual_coding_disabled_flag : " << ( tmp_sh_ts_residual_coding_disabled_flag ? "1" : "0");
      }
      bool tmp_sps_ts_residual_coding_rice_present_in_sh_flag = false; // i can t evaluate this value at this moment
      // we can get this in sps_range_extension section or SpsRangeExtension func
      //sps->sps_ts_residual_coding_rice_present_in_sh_flag

      if( !slice_header->sh_ts_residual_coding_disabled_flag && tmp_sps_ts_residual_coding_rice_present_in_sh_flag ){
        int tmp_sh_ts_residual_coding_rice_idx_minus1 = 0;
        TRUE_OR_RETURN(br->ReadBits(3,&tmp_sh_ts_residual_coding_rice_idx_minus1));
        slice_header->sh_ts_residual_coding_rice_idx_minus1 = tmp_sh_ts_residual_coding_rice_idx_minus1;
        LOG(INFO) << " # sh_ts_residual_coding_rice_idx_minus1 : " << tmp_sh_ts_residual_coding_rice_idx_minus1;
      }
      bool tmp_sps_reverse_last_sig_coeff_enabled_flag = false;
      //if( sps->sps_reverse_last_sig_coeff_enabled_flag ){
      // we can get this in sps_range_extension section or SpsRangeExtension func
      if( tmp_sps_reverse_last_sig_coeff_enabled_flag ){

        bool tmp_sh_reverse_last_sig_coeff_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_sh_reverse_last_sig_coeff_flag));
        slice_header->sh_reverse_last_sig_coeff_flag = tmp_sh_reverse_last_sig_coeff_flag;
        LOG(INFO) << " # sh_reverse_last_sig_coeff_flag : " << ( tmp_sh_reverse_last_sig_coeff_flag ? "1" : "0");
      }

      if( pps->pps_slice_header_extension_present_flag ) {
        int tmp_sh_slice_header_extension_length = 0;
        TRUE_OR_RETURN(br->ReadUE(&tmp_sh_slice_header_extension_length));
        slice_header->sh_slice_header_extension_length = tmp_sh_slice_header_extension_length;
        LOG(INFO) << " # sh_slice_header_extension_length : " << tmp_sh_slice_header_extension_length;

        int tmp_sh_slice_header_extension_data_byte = 0;
        for(int i = 0; i < slice_header->sh_slice_header_extension_length; i++){
          TRUE_OR_RETURN(br->ReadBits(8,&tmp_sh_slice_header_extension_data_byte));
          slice_header->sh_slice_header_extension_data_byte.push_back(tmp_sh_slice_header_extension_data_byte);
          LOG(INFO) << " # sh_slice_header_extension_data_byte[ " << i << " ] : " << tmp_sh_slice_header_extension_data_byte;
        }
      }




                      /**************************NumEntryPoints, eq 141 page 160******************************************************************************* */

                      // NumCtusInCurrSlice   need evaluate  eq 113 page 153
                      if( pps->pps_rect_slice_flag ) {

                          int picLevelSliceIdx = slice_header->sh_slice_address;
                          for( int j = 0; j < slice_header->CurrSubpicIdx; j++ ){
                            picLevelSliceIdx += slice_header->NumSlicesInSubpic[j];
                          }
                          slice_header->NumCtusInCurrSlice = slice_header->NumCtusInSlice[ picLevelSliceIdx ];
                          for( int i = 0; i < slice_header->NumCtusInCurrSlice; i++ ){
                            slice_header->CtbAddrInCurrSlice[ i ] = slice_header->CtbAddrInSlice[ picLevelSliceIdx ][i];
                          }
                      } else {
                            slice_header->NumCtusInCurrSlice = 0;
                            int sum_slice = slice_header->sh_slice_address + slice_header->sh_num_tiles_in_slice_minus1;

                            for( uint32_t tileIdx = slice_header->sh_slice_address; tileIdx <= static_cast<uint32_t>(sum_slice) ; tileIdx++ ){
                                  int tileX = static_cast<int>(tileIdx % pps->NumTileColumns);
                                  int tileY = static_cast<int>(tileIdx / pps->NumTileColumns);
                                  for( uint32_t ctbY = slice_header->TileRowBdVal[ tileY ]; ctbY < slice_header->TileRowBdVal[ tileY+1 ]; ctbY++ ) {
                                      for( int ctbX = static_cast<int>(slice_header->TileColBdVal[ tileX ]); ctbX < static_cast<int>(slice_header->TileColBdVal[ tileX + 1 ]); ctbX++ ) {
                                          slice_header->CtbAddrInCurrSlice[ slice_header->NumCtusInCurrSlice ] = static_cast<int>(ctbY) * slice_header->PicWidthInCtbsY + ctbX;
                                          slice_header->NumCtusInCurrSlice++;
                                      }
                                  }
                            }
                        }
                        //

                      /*****************CtbToTileColBd        eq 18 page 29             ********** */
                      int tileX = 0;
                      for( int ctbAddrX = 0; ctbAddrX <= slice_header->PicWidthInCtbsY; ctbAddrX++ ) {
                        if( static_cast<uint32_t>(ctbAddrX) == slice_header->TileColBdVal[ tileX + 1 ] ){
                          tileX++;
                        }
                        slice_header->CtbToTileColBd[ ctbAddrX ] = slice_header->TileColBdVal[ tileX ];
                        slice_header->ctbToTileColIdx[ ctbAddrX ] = tileX;
                      }

                      /***************************************************************************************************  */
                      //CtbAddrInCurrSlice need evaluate   OK
                      // CtbAddrInCurrSlice need evalate  OK
                      //CtbToTileColBd    ok
                      //int NumEntryPoints = 0;

                      if( sps->sps_entry_point_offsets_present_flag ){

                        for( int i = 1; i < slice_header->NumCtusInCurrSlice; i++ ) {

                          int ctbAddrX = slice_header->CtbAddrInCurrSlice[ i ] % slice_header->PicWidthInCtbsY;
                          int ctbAddrY = slice_header->CtbAddrInCurrSlice[ i ] / slice_header->PicWidthInCtbsY;
                          int prevCtbAddrX = slice_header->CtbAddrInCurrSlice[ i-1 ] % slice_header->PicWidthInCtbsY;
                          int prevCtbAddrY = slice_header->CtbAddrInCurrSlice[ i-1 ] / slice_header->PicWidthInCtbsY;
                          if( slice_header->CtbToTileRowBd[ ctbAddrY ] != slice_header->CtbToTileRowBd[ prevCtbAddrY ] ||
                            slice_header->CtbToTileColBd[ ctbAddrX ] != slice_header->CtbToTileColBd[ prevCtbAddrX ] ||
                            ( ctbAddrY != prevCtbAddrY && sps->sps_entropy_coding_sync_enabled_flag ) ){
                              slice_header->NumEntryPoints++;
                            }
                      }
                    }

       /********************************************************************************************************************************/
      if( slice_header->NumEntryPoints > 0 ) {
        int tmp_sh_entry_offset_len_minus1 = 0;
        TRUE_OR_RETURN(br->ReadUE(&tmp_sh_entry_offset_len_minus1));
        LOG(INFO) << " # sh_entry_offset_len_minus1 : " << tmp_sh_entry_offset_len_minus1;
        slice_header->sh_entry_offset_len_minus1 = tmp_sh_entry_offset_len_minus1;
        for( int i = 0; i < slice_header->NumEntryPoints; i++ ){
          int len_sh_entry_point_offset_minus1 = 0;
          int tmp_sh_entry_point_offset_minus1 = 0;

          TRUE_OR_RETURN(br->ReadBits(len_sh_entry_point_offset_minus1,&tmp_sh_entry_point_offset_minus1));
          slice_header->sh_entry_point_offset_minus1.push_back(tmp_sh_entry_point_offset_minus1);
          LOG(INFO) << " # sh_entry_point_offset_minus1[ " << i << " ] : " << tmp_sh_entry_point_offset_minus1;
        }
      }
  OK_OR_RETURN(ByteAlignment(br));
  // Ensure the slice header is byte-aligned before finishing parsing.
  slice_header->header_bit_size = static_cast<int>(nalu.payload_size() * 8) - br->NumBitsLeft();
  LOG(INFO) << " # Slice header size (bits): " << slice_header->header_bit_size;
  return kOk;
}


#endif




H266Parser::Result H266Parser::ParsePps(const Nalu& nalu, int* pps_id) {
  DCHECK_EQ(Nalu::H266_PPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 PPS NALU";

  //warning  with }} for close

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;
  uint32_t NumTileColumns = 0;
  uint32_t NumTileRows = 0;

  *pps_id = -1;
   std::unique_ptr<H266Pps> pps(new H266Pps);
  /*std::unique_ptr<H266Sps> sps(new H266Sps); */

  //pic_parameter_set_rbsp( ) 7.3.2.5
  int tmp_pic_parameter_set_id = 0;
  TRUE_OR_RETURN(br->ReadBits(6, &tmp_pic_parameter_set_id));  // 6 bits
  pps->pic_parameter_set_id = tmp_pic_parameter_set_id;
  DLOG(INFO) << "## pic_parameter_set_id: " << pps->pic_parameter_set_id;

  int tmp_seq_parameter_set_id = 0;
  TRUE_OR_RETURN(br->ReadBits(4,&tmp_seq_parameter_set_id));  // 4 bits
  pps->seq_parameter_set_id = tmp_seq_parameter_set_id;
  DLOG(INFO) << "## seq_parameter_set_id : " << pps->seq_parameter_set_id;



  bool tmp_pps_mixed_nalu_types_in_pic_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_pps_mixed_nalu_types_in_pic_flag));
  pps->pps_mixed_nalu_types_in_pic_flag = tmp_pps_mixed_nalu_types_in_pic_flag;

  int tmp_pps_pic_width_in_luma_samples = 0;
  TRUE_OR_RETURN(br->ReadUE(&tmp_pps_pic_width_in_luma_samples));
  pps->pps_pic_width_in_luma_samples = tmp_pps_pic_width_in_luma_samples;
  DLOG(INFO) << "## pps_pic_width_in_luma_samples : " << tmp_pps_pic_width_in_luma_samples;


  int tmp_pps_pic_height_in_luma_samples = 0;
  TRUE_OR_RETURN(br->ReadUE(&tmp_pps_pic_height_in_luma_samples));
  pps->pps_pic_height_in_luma_samples = tmp_pps_pic_height_in_luma_samples;
  DLOG(INFO) << "## pps_pic_height_in_luma_samples: " << tmp_pps_pic_height_in_luma_samples;


  bool tmp_pps_conformance_window_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_pps_conformance_window_flag));
  pps->pps_conformance_window_flag = tmp_pps_conformance_window_flag;
  DLOG(INFO) << "## pps_conformance_window_flag : " << ( tmp_pps_conformance_window_flag ? "1" : "0");


  if(pps->pps_conformance_window_flag){
    int tmp_pps_conf_win_left_offset = 0;
    TRUE_OR_RETURN(br->ReadUE(&tmp_pps_conf_win_left_offset));
    pps->pps_conf_win_left_offset = tmp_pps_conf_win_left_offset;

    int tmp_pps_conf_win_right_offset = 0;
    TRUE_OR_RETURN((br->ReadUE(&tmp_pps_conf_win_right_offset)));
    pps->pps_conf_win_right_offset = tmp_pps_conf_win_right_offset;

    int tmp_pps_conf_win_top_offset = 0;
    TRUE_OR_RETURN((br->ReadUE(&tmp_pps_conf_win_top_offset)));
    pps->pps_conf_win_top_offset = tmp_pps_conf_win_top_offset;

    int tmp_pps_conf_win_bottom_offset = 0;
    TRUE_OR_RETURN((br->ReadUE(&tmp_pps_conf_win_bottom_offset)));
    pps->pps_conf_win_bottom_offset = tmp_pps_conf_win_bottom_offset;

  }
  bool tmp_pps_scaling_window_explicit_signalling_flag = false;
  TRUE_OR_RETURN((br->ReadBool(&tmp_pps_scaling_window_explicit_signalling_flag)));
  pps->pps_scaling_window_explicit_signalling_flag = tmp_pps_scaling_window_explicit_signalling_flag;
  DLOG(INFO) << "## pps_scaling_window_explicit_signalling_flag : " << (tmp_pps_scaling_window_explicit_signalling_flag ? "1" : "0");



  if(pps->pps_scaling_window_explicit_signalling_flag){
    int tmp_pps_scaling_win_left_offset = 0;
    TRUE_OR_RETURN((br->ReadSE(&tmp_pps_scaling_win_left_offset)));
    pps->pps_scaling_win_left_offset = tmp_pps_scaling_win_left_offset;

    int tmp_pps_scaling_win_right_offset = 0;
    TRUE_OR_RETURN((br->ReadSE(&tmp_pps_scaling_win_right_offset)));
    pps->pps_scaling_win_right_offset = tmp_pps_scaling_win_right_offset;

    int tmp_pps_scaling_win_top_offset = 0;
    TRUE_OR_RETURN((br->ReadSE(&tmp_pps_scaling_win_top_offset)));
    pps->pps_scaling_win_top_offset = tmp_pps_scaling_win_top_offset;

    int tmp_pps_scaling_win_bottom_offset = 0;
    TRUE_OR_RETURN((br->ReadSE(&tmp_pps_scaling_win_bottom_offset)));
    pps->pps_scaling_win_bottom_offset = tmp_pps_scaling_win_bottom_offset;

  }
  bool tmp_pps_output_flag_present_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_pps_output_flag_present_flag));
  DLOG(INFO) << "## pps_output_flag_present_flag : " << (tmp_pps_output_flag_present_flag ? "1" : "0");

  bool tmp_pps_no_pic_partition_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_pps_no_pic_partition_flag));
  pps->pps_no_pic_partition_flag = tmp_pps_no_pic_partition_flag;
  DLOG(INFO) << "## pps_no_pic_partition_flag : " << ( tmp_pps_no_pic_partition_flag ? "1" : "0");

  bool tmp_pps_subpic_id_mapping_present_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_pps_subpic_id_mapping_present_flag));
  pps->pps_subpic_id_mapping_present_flag = tmp_pps_subpic_id_mapping_present_flag;

  DLOG(INFO) << "## pps_subpic_id_mapping_present_flag : " << (tmp_pps_subpic_id_mapping_present_flag ? "1" : "0");
    DLOG(INFO) << "##  WE NEED ACCESS TO PPS to get : " << pps->seq_parameter_set_id;


#if 0
  //sps->sps_seq_parameter_set_id
  H266Sps* sps = GetSps(pps->seq_parameter_set_id);
    if(!sps){
      DLOG(INFO) << "## We don t found sps instance from seq_parameter_set_id :" << pps->seq_parameter_set_id;
    }
    TRUE_OR_RETURN(sps);
 
   
#else 
 // have a coredump
    DebugPrintAvailableSets();


    if (!HasSps(pps->pps_seq_parameter_set_id)) {
        DLOG(ERROR) << "SPS " << pps->pps_seq_parameter_set_id << " not found";
        DebugPrintAvailableSets();
        return kInvalidStream;
    }
    H266Sps* sps = GetSps(pps->pps_seq_parameter_set_id);
   if (!sps) {
     DLOG(ERROR) << "GetSps returned nullptr";
     return kInvalidStream;
   }
   if (!sps) {
     DLOG(WARNING) << "Trying first available SPS";
     sps = GetFirstSps();
     if (!sps) {
       DLOG(ERROR) << "No SPS available at all";
       return kInvalidStream;
     }
   }
    TRUE_OR_RETURN(sps);
#endif 




  if(pps->pps_subpic_id_mapping_present_flag){
    if(!pps->pps_no_pic_partition_flag){
      int tmp_pps_num_subpics_minus1 = 0;
      TRUE_OR_RETURN((br->ReadUE(&tmp_pps_num_subpics_minus1)));
      pps->pps_num_subpics_minus1 = tmp_pps_num_subpics_minus1;
      DLOG(INFO) << "## pps_num_subpics_minus1 : " << tmp_pps_num_subpics_minus1;


      NumTileColumns = 1;
      NumTileRows = 1;
    }
    int tmp_pps_subpic_id_len_minus1 = 0;
    TRUE_OR_RETURN((br->ReadUE(&tmp_pps_subpic_id_len_minus1)));
    pps->pps_subpic_id_len_minus1 = tmp_pps_subpic_id_len_minus1;
    DLOG(INFO) << "## pps_subpic_id_len_minus1: " << tmp_pps_subpic_id_len_minus1;

    /***************************************** */

    DLOG(INFO) << "##  WE NEED ACESS TO PPS to get : " << pps->seq_parameter_set_id;

    #if 0

    if ( pps->seq_parameter_set_id > 0){
      H266Vps* vps = GetVps(pps->seq_parameter_set_id);
      if(!vps){
          DLOG(INFO) << "## We don t found vps instance from seq_parameter_set_id :" << pps->seq_parameter_set_id;
      }
      TRUE_OR_RETURN(vps);
      if (vps && (sps->sps_video_parameter_set_id > vps->vps_max_sublayers) ){
         DLOG(INFO) << "## If sps_video_parameter_set_id is greater than 0, the value of sps_max_sublayers_minus1 shall be in the range of 0 to vps_max_sublayers_minus1, "
         "inclusive ";
      }
    }


    #endif




    /***************************************** */




    for(int i = 0;i <= pps->pps_num_subpics_minus1;i++){
      uint32_t tmp_pps_subpic_id = 0;
      TRUE_OR_RETURN(br->ReadBits(sps->sps_subpic_id_len_minus1, &tmp_pps_subpic_id));
      pps->pps_subpic_id.push_back(tmp_pps_subpic_id);
        DLOG(INFO) << "## pps_subpic_id : " << tmp_pps_subpic_id;

    }
  }


  if(!pps->pps_no_pic_partition_flag){
    int tmp_pps_log2_ctu_size_minus5 = 0;
    TRUE_OR_RETURN(br->ReadBits(2,&tmp_pps_log2_ctu_size_minus5));
    pps->pps_log2_ctu_size_minus5 = tmp_pps_log2_ctu_size_minus5;  // 2 bits
    DLOG(INFO) << "## pps_log2_ctu_size_minus5 : " << tmp_pps_log2_ctu_size_minus5;

    int tmp_pps_num_exp_tile_columns_minus1 = 0;
    TRUE_OR_RETURN(br->ReadUE(&tmp_pps_num_exp_tile_columns_minus1));
    pps->pps_num_exp_tile_columns_minus1 = tmp_pps_num_exp_tile_columns_minus1;
    DLOG(INFO) << "## pps_num_exp_tile_columns_minus1 : " << tmp_pps_num_exp_tile_columns_minus1;

     /************************************************** */

    int tmp_pps_num_exp_tile_rows_minus1 = 0;
    TRUE_OR_RETURN(br->ReadUE(&pps->pps_num_exp_tile_rows_minus1));
    pps->pps_num_exp_tile_rows_minus1 = tmp_pps_num_exp_tile_rows_minus1;
    DLOG(INFO) << "## pps_num_exp_tile_rows_minus1 : " << tmp_pps_num_exp_tile_rows_minus1;

    for( int i = 0; i <= pps->pps_num_exp_tile_columns_minus1; i++ ){
      uint32_t tmp_pps_tile_column_width_minus1 = 0;
      TRUE_OR_RETURN(br->ReadUE(&tmp_pps_tile_column_width_minus1));
      pps->pps_tile_column_width_minus1.push_back(tmp_pps_tile_column_width_minus1);
      DLOG(INFO) << "## pps_tile_column_width_minus1[ " << i << " ] : " << tmp_pps_tile_column_width_minus1;
    }
    for( int i = 0; i <= pps->pps_num_exp_tile_rows_minus1; i++ ){
      uint32_t tmp_pps_tile_row_height_minus1 = 0;
      TRUE_OR_RETURN(br->ReadUE(&tmp_pps_tile_row_height_minus1));
      pps->pps_tile_row_height_minus1.push_back(tmp_pps_tile_row_height_minus1);
      DLOG(INFO) << "## pps_tile_row_height_minus1[ " << i << " ] : " << tmp_pps_tile_row_height_minus1;
    }
    int CtbSizeY = 1 << (pps->pps_log2_ctu_size_minus5 + 5);
    int PicWidthInCtbsY = ceil((float)pps->pps_pic_width_in_luma_samples / CtbSizeY);
    DLOG(INFO) << "## PicWidthInCtbsY : " << PicWidthInCtbsY;
    int PicHeightInCtbsY = ceil((float)pps->pps_pic_height_in_luma_samples / CtbSizeY);
    DLOG(INFO) << "## PicHeightInCtbsY : " << PicHeightInCtbsY;
    // Backup for sps and slice_header
    pps->CtbSizeY = CtbSizeY;
    DLOG(INFO) << "## CtbSizeY from PPS : " << CtbSizeY; 

    //pps->CtbSizeY = CtbSizeY;

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

    if( pps->NumTilesInPic > 1 ) {
        bool tmp_pps_loop_filter_across_tiles_enabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_loop_filter_across_tiles_enabled_flag));
        pps->pps_loop_filter_across_tiles_enabled_flag = tmp_pps_loop_filter_across_tiles_enabled_flag;
        DLOG(INFO) << "## pps_loop_filter_across_tiles_enabled_flag : " << ( tmp_pps_loop_filter_across_tiles_enabled_flag ? "1" : "0");
        bool tmp_pps_rect_slice_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_rect_slice_flag));
        pps->pps_rect_slice_flag = tmp_pps_rect_slice_flag;
        DLOG(INFO) << "## pps_rect_slice_flag : " << ( tmp_pps_rect_slice_flag ? "1" : "0");
    }
    if(pps->pps_single_slice_per_subpic_flag){
      bool tmp_pps_subpic_id_mapping_present_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_pps_subpic_id_mapping_present_flag));
      pps->pps_subpic_id_mapping_present_flag = tmp_pps_subpic_id_mapping_present_flag;
      DLOG(INFO) << "## pps_subpic_id_mapping_present_flag : " << ( tmp_pps_subpic_id_mapping_present_flag ? "1" : "0");
    }
    if( pps->pps_rect_slice_flag && !pps->pps_single_slice_per_subpic_flag ) {
      int tmp_pps_num_slices_in_pic_minus1 = 0;
      TRUE_OR_RETURN(br->ReadUE(&tmp_pps_num_slices_in_pic_minus1));
      pps->pps_num_slices_in_pic_minus1 = tmp_pps_num_slices_in_pic_minus1;
      DLOG(INFO) << "## pps_num_slices_in_pic_minus1 : " << tmp_pps_num_slices_in_pic_minus1;

      if(pps->pps_num_slices_in_pic_minus1 > 1 ){
          bool tmp_pps_tile_idx_delta_present_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_tile_idx_delta_present_flag));
          pps->pps_tile_idx_delta_present_flag = tmp_pps_tile_idx_delta_present_flag;
          DLOG(INFO) << "## pps_tile_idx_delta_present_flag : " << ( tmp_pps_tile_idx_delta_present_flag ? "1" : "0");
      }
      // #### I don't know populate this variable     check slice header
      std::vector<uint32_t> SliceTopLeftTileIdx; // I don't know populate this variable






      //int tmp_pps_slice_height_in_tiles_minus1 = 0;
        std::vector<int> RowHeightVal;
        int remainingHeightInCtbsY;
        int CtbLog2SizeY = sps->sps_log2_ctu_size_minus5 + 5;
        int CtbSizeY = 1 << CtbLog2SizeY;

        int PicHeightInCtbsY = ceil( pps->pps_pic_height_in_luma_samples / CtbSizeY );
        int jj = 0;

        remainingHeightInCtbsY = PicHeightInCtbsY;
        for( int jj = 0; jj <= pps->pps_num_exp_tile_rows_minus1; jj++ ) {
          RowHeightVal.push_back(pps->pps_tile_row_height_minus1[jj] + 1);
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

      for( int i = 0; i < pps->pps_num_slices_in_pic_minus1; i++ ) {
        if( SliceTopLeftTileIdx[ i ] % NumTileColumns != NumTileColumns-1 ){
          int tmp_pps_slice_width_in_tiles_minus1 = 0;
          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_slice_width_in_tiles_minus1));
          pps->pps_slice_width_in_tiles_minus1.push_back(tmp_pps_slice_width_in_tiles_minus1);

        }
        if( static_cast<int>(SliceTopLeftTileIdx[ i ] / NumTileColumns) != NumTileRows-1 && ( pps->pps_tile_idx_delta_present_flag || static_cast<int>(SliceTopLeftTileIdx[ i ] % NumTileColumns) == 0 ) ){
          int tmp_pps_slice_height_in_tiles_minus1 = 0;
          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_slice_height_in_tiles_minus1));
          pps->pps_slice_height_in_tiles_minus1.push_back(tmp_pps_slice_height_in_tiles_minus1);
        }

        if( pps->pps_slice_width_in_tiles_minus1[ i ] == 0 && pps->pps_slice_height_in_tiles_minus1[ i ] == 0 && RowHeightVal[ SliceTopLeftTileIdx[ i ] / NumTileColumns ] > 1 ) {
          u_int tmp_pps_num_exp_slices_in_tile = 0;

          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_num_exp_slices_in_tile));
          pps->pps_num_exp_slices_in_tile.push_back(tmp_pps_num_exp_slices_in_tile);
          // not sure how to populate this variable
          std::vector<uint32_t> NumSlicesInTile;
          NumSlicesInTile.assign(pps->NumTilesInPic, 0);
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
            DLOG(INFO) << "## pps_exp_slice_height_in_ctus_minus1[ " << i << " ][ " << j << " ] : " << tmp_pps_exp_slice_height_in_ctus_minus1;

            //i += NumSlicesInTile[i] -1;
            int numSlices = NumSlicesInTile[i];
            i += numSlices - 1;


          }
          if( pps->pps_tile_idx_delta_present_flag && i < pps->pps_num_slices_in_pic_minus1 ){
            int tmp_pps_tile_idx_delta_val = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_tile_idx_delta_val));
            pps->pps_tile_idx_delta_val.push_back(tmp_pps_tile_idx_delta_val);
            DLOG(INFO) << "## pps_tile_idx_delta_val[ " << i << " ] : " << tmp_pps_tile_idx_delta_val;
          }

        }
        if( !pps->pps_rect_slice_flag || pps->pps_single_slice_per_subpic_flag || pps->pps_num_slices_in_pic_minus1 > 0 ){
          bool tmp_pps_loop_filter_across_slices_enabled_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_loop_filter_across_slices_enabled_flag));
          pps->pps_loop_filter_across_slices_enabled_flag = tmp_pps_loop_filter_across_slices_enabled_flag;
          DLOG(INFO) << "## pps_loop_filter_across_slices_enabled_flag : " << ( tmp_pps_loop_filter_across_slices_enabled_flag ? "1" : "0");
        }
        bool tmp_pps_cabac_init_present_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_cabac_init_present_flag));
        pps->pps_cabac_init_present_flag = tmp_pps_cabac_init_present_flag;
        DLOG(INFO) << "## pps_cabac_init_present_flag: " << ( tmp_pps_cabac_init_present_flag ? "1" : "0");


        for( i = 0; i < 2; i++ ) {
          int tmp_pps_num_ref_idx_default_active_minus1= 0;
          TRUE_OR_RETURN(br->ReadSE(&tmp_pps_num_ref_idx_default_active_minus1));
          pps->pps_num_ref_idx_default_active_minus1.push_back(tmp_pps_num_ref_idx_default_active_minus1);
          DLOG(INFO) << "## pps_num_ref_idx_default_active_minus1: " << ( tmp_pps_num_ref_idx_default_active_minus1 ? "1" : "0");


        }
        bool tmp_pps_rpl1_idx_present_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_rpl1_idx_present_flag));
        pps->pps_rpl1_idx_present_flag = tmp_pps_rpl1_idx_present_flag;
        DLOG(INFO) << "## pps_rpl1_idx_present_flag : " << ( tmp_pps_rpl1_idx_present_flag ? "1" : "0");

        bool tmp_pps_weighted_pred_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_weighted_pred_flag));
        pps->pps_weighted_pred_flag = tmp_pps_weighted_pred_flag;
        DLOG(INFO) << "## pps_weighted_pred_flag : " << ( tmp_pps_weighted_pred_flag ? "1" : "0");

        bool tmp_pps_weighted_bipred_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_weighted_bipred_flag));
        pps->pps_weighted_bipred_flag = tmp_pps_weighted_bipred_flag;
        DLOG(INFO) << "## pps_weighted_bipred_flag : " << ( tmp_pps_weighted_bipred_flag ? "1" : "0");

        bool tmp_pps_ref_wraparound_enabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_ref_wraparound_enabled_flag));
        pps->pps_ref_wraparound_enabled_flag = tmp_pps_ref_wraparound_enabled_flag;
        DLOG(INFO) << "## pps_ref_wraparound_enabled_flag : " << ( tmp_pps_ref_wraparound_enabled_flag ? "1" : "0");

        if( pps->pps_ref_wraparound_enabled_flag ) {
          int tmp_pps_pic_width_minus_wraparound_offset = 0;
          TRUE_OR_RETURN(br->ReadUE(&tmp_pps_pic_width_minus_wraparound_offset));
          pps->pps_pic_width_minus_wraparound_offset = tmp_pps_pic_width_minus_wraparound_offset;
          DLOG(INFO) << "## pps_pic_width_minus_wraparound_offset : " << tmp_pps_pic_width_minus_wraparound_offset;
        }
        int tmp_pps_init_qp_minus26 = 0;
        TRUE_OR_RETURN(br->ReadSE(&tmp_pps_init_qp_minus26));
        pps->pps_init_qp_minus26 = tmp_pps_init_qp_minus26;
        DLOG(INFO) << "## pps_init_qp_minus26 : " << tmp_pps_init_qp_minus26;

        bool tmp_pps_cu_qp_delta_enabled_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_cu_qp_delta_enabled_flag));
        pps->pps_cu_qp_delta_enabled_flag = tmp_pps_cu_qp_delta_enabled_flag;
        DLOG(INFO) << "## pps_cu_qp_delta_enabled_flag : " << ( tmp_pps_cu_qp_delta_enabled_flag ? "1" : "0");

        bool tmp_pps_chroma_tool_offsets_present_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_chroma_tool_offsets_present_flag));
        pps->pps_chroma_tool_offsets_present_flag = tmp_pps_chroma_tool_offsets_present_flag;
        DLOG(INFO) << "## pps_chroma_tool_offsets_present_flag : " << ( tmp_pps_chroma_tool_offsets_present_flag ? "1" : "0");

        if( pps->pps_chroma_tool_offsets_present_flag ) {
          int tmp_pps_cb_qp_offset = 0;
          TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cb_qp_offset));
          pps->pps_cb_qp_offset = tmp_pps_cb_qp_offset;
          DLOG(INFO) << "## pps_cb_qp_offset : " << tmp_pps_cb_qp_offset;

          int tmp_pps_cr_qp_offset = 0;
          TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cr_qp_offset));
          pps->pps_cr_qp_offset = tmp_pps_cr_qp_offset;
          DLOG(INFO) << "## pps_cr_qp_offset : " << tmp_pps_cr_qp_offset;
          bool tmp_pps_joint_cbcr_qp_offset_present_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_joint_cbcr_qp_offset_present_flag));
          pps->pps_joint_cbcr_qp_offset_present_flag = tmp_pps_joint_cbcr_qp_offset_present_flag;
          DLOG(INFO) << "## pps_joint_cbcr_qp_offset_present_flag : " << ( tmp_pps_joint_cbcr_qp_offset_present_flag ? "1" : "0");

          if( pps->pps_joint_cbcr_qp_offset_present_flag ) {
            int tmp_pps_joint_cbcr_qp_offset_value = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_joint_cbcr_qp_offset_value));
            pps->pps_joint_cbcr_qp_offset_value = tmp_pps_joint_cbcr_qp_offset_value;
            DLOG(INFO) << "## pps_joint_cbcr_qp_offset_value : " << tmp_pps_joint_cbcr_qp_offset_value;
          }
          bool tmp_pps_slice_chroma_qp_offsets_present_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_slice_chroma_qp_offsets_present_flag));
          pps->pps_slice_chroma_qp_offsets_present_flag = tmp_pps_slice_chroma_qp_offsets_present_flag;
          DLOG(INFO) << "## pps_slice_chroma_qp_offsets_present_flag : " << ( tmp_pps_slice_chroma_qp_offsets_present_flag ? "1" : "0");

          bool tmp_pps_cu_chroma_qp_offset_list_enabled_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_cu_chroma_qp_offset_list_enabled_flag));
          pps->pps_cu_chroma_qp_offset_list_enabled_flag = tmp_pps_cu_chroma_qp_offset_list_enabled_flag;
          DLOG(INFO) << "## pps_cu_chroma_qp_offset_list_enabled_flag : " << ( tmp_pps_cu_chroma_qp_offset_list_enabled_flag ? "1" : "0");

          if( pps->pps_cu_chroma_qp_offset_list_enabled_flag ) {
            int tmp_pps_cu_chroma_qp_offset_list_len_minus1 = 0;
            TRUE_OR_RETURN(br->ReadUE(&tmp_pps_cu_chroma_qp_offset_list_len_minus1));
            pps->pps_cu_chroma_qp_offset_list_len_minus1 = tmp_pps_cu_chroma_qp_offset_list_len_minus1;
            DLOG(INFO) << "## pps_cu_chroma_qp_offset_list_len_minus1 : " << tmp_pps_cu_chroma_qp_offset_list_len_minus1;

          }
          for( int i = 0; i <= pps->pps_chroma_qp_offset_list_len_minus1; i++ ){
            int tmp_pps_cb_qp_offset_list = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cb_qp_offset_list));
            pps->pps_cb_qp_offset_list.push_back(tmp_pps_cb_qp_offset_list);
            DLOG(INFO) << "## pps_cb_qp_offset_list[ " << i << " ] : " << tmp_pps_cb_qp_offset_list;

            int tmp_pps_cr_qp_offset_list = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cr_qp_offset_list));
            pps->pps_cr_qp_offset_list.push_back(tmp_pps_cr_qp_offset_list);
            DLOG(INFO) << "## pps_cr_qp_offset_list[ " << i << " ] : " << tmp_pps_cr_qp_offset_list;

            if( pps->pps_joint_cbcr_qp_offset_present_flag ) {

              int tmp_pps_joint_cbcr_qp_offset_list = 0;
              TRUE_OR_RETURN(br->ReadSE(&tmp_pps_joint_cbcr_qp_offset_list));
              pps->pps_joint_cbcr_qp_offset_list.push_back(tmp_pps_joint_cbcr_qp_offset_list);
              DLOG(INFO) << "## pps_joint_cbcr_qp_offset_list[ " << i << " ] : " << tmp_pps_joint_cbcr_qp_offset_list;
            }
          }
        } else {
          pps->pps_cb_qp_offset = 0;
          pps->pps_cr_qp_offset = 0;
          pps->pps_joint_cbcr_qp_offset_present_flag = false;
          pps->pps_joint_cbcr_qp_offset_value = 0;
          pps->pps_slice_chroma_qp_offsets_present_flag = false;
          pps->pps_cu_chroma_qp_offset_list_enabled_flag = false;
        }

        bool tmp_pps_deblocking_filter_control_present_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_deblocking_filter_control_present_flag));
        pps->pps_deblocking_filter_control_present_flag = tmp_pps_deblocking_filter_control_present_flag;
        DLOG(INFO) << "## pps_deblocking_filter_control_present_flag : " << ( tmp_pps_deblocking_filter_control_present_flag ? "1" : "0");

        if( pps->pps_deblocking_filter_control_present_flag ) {
          bool tmp_pps_deblocking_filter_override_enabled_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_deblocking_filter_override_enabled_flag));
          pps->pps_deblocking_filter_override_enabled_flag = tmp_pps_deblocking_filter_override_enabled_flag;
          DLOG(INFO) << "## pps_deblocking_filter_override_enabled_flag : " << ( tmp_pps_deblocking_filter_override_enabled_flag ? "1" : "0");
          bool tmp_pps_deblocking_filter_disabled_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_deblocking_filter_disabled_flag));
          pps->pps_deblocking_filter_disabled_flag = tmp_pps_deblocking_filter_disabled_flag;
          DLOG(INFO) << "## pps_deblocking_filter_disabled_flag : " << ( tmp_pps_deblocking_filter_disabled_flag ? "1" : "0");

          if( !pps->pps_no_pic_partition_flag && pps->pps_deblocking_filter_override_enabled_flag ) {
            bool tmp_pps_dbf_info_in_ph_flag = false;
            TRUE_OR_RETURN(br->ReadBool(&tmp_pps_dbf_info_in_ph_flag));
            pps->pps_dbf_info_in_ph_flag = tmp_pps_dbf_info_in_ph_flag;
            DLOG(INFO) << "## pps_dbf_info_in_ph_flag : " << ( tmp_pps_dbf_info_in_ph_flag ? "1" : "0");
          }
          if( !pps->pps_deblocking_filter_disabled_flag ) {
            int tmp_pps_luma_beta_offset_div2 = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_luma_beta_offset_div2));
            pps->pps_luma_beta_offset_div2 = tmp_pps_luma_beta_offset_div2;
            DLOG(INFO) << "## pps_luma_beta_offset_div2 : " << tmp_pps_luma_beta_offset_div2;
            int tmp_pps_luma_tc_offset_div2 = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_pps_luma_tc_offset_div2));
            pps->pps_luma_tc_offset_div2 = tmp_pps_luma_tc_offset_div2;
            DLOG(INFO) << "## pps_luma_tc_offset_div2 : " << tmp_pps_luma_tc_offset_div2;

            if( pps->chroma_tool_offsets_present_flag ) {
              int tmp_pps_cb_beta_offset_div2 = 0;
              TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cb_beta_offset_div2));
              pps->pps_cb_beta_offset_div2 = tmp_pps_cb_beta_offset_div2;
              DLOG(INFO) << "## pps_cb_beta_offset_div2 : " << tmp_pps_cb_beta_offset_div2;
              int tmp_pps_cb_tc_offset_div2 = 0;
              TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cb_tc_offset_div2));
              pps->pps_cb_tc_offset_div2 = tmp_pps_cb_tc_offset_div2;
              DLOG(INFO) << "## pps_cb_tc_offset_div2 : " << tmp_pps_cb_tc_offset_div2;
              int tmp_pps_cr_beta_offset_div2 = 0;
              TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cr_beta_offset_div2));
              pps->pps_cr_beta_offset_div2 = tmp_pps_cr_beta_offset_div2;
              DLOG(INFO) << "## pps_cr_beta_offset_div2 : " << tmp_pps_cr_beta_offset_div2;
              int tmp_pps_cr_tc_offset_div2 = 0;
              TRUE_OR_RETURN(br->ReadSE(&tmp_pps_cr_tc_offset_div2));
              pps->pps_cr_tc_offset_div2 = tmp_pps_cr_tc_offset_div2;
              DLOG(INFO) << "## pps_cr_tc_offset_div2 : " << tmp_pps_cr_tc_offset_div2;

            } else {
              pps->pps_cb_beta_offset_div2 = 0;
              pps->pps_cb_tc_offset_div2 = 0;
              pps->pps_cr_beta_offset_div2 = 0;
              pps->pps_cr_tc_offset_div2 = 0;
            }

          } else {
            pps->pps_luma_beta_offset_div2 = 0;
            pps->pps_luma_tc_offset_div2 = 0;
            pps->pps_cb_beta_offset_div2 = 0;
            pps->pps_cb_tc_offset_div2 = 0;
            pps->pps_cr_beta_offset_div2 = 0;
            pps->pps_cr_tc_offset_div2 = 0;
          }

      } else {
        pps->pps_deblocking_filter_override_enabled_flag = false;
        pps->pps_deblocking_filter_disabled_flag = false;
        pps->pps_dbf_info_in_ph_flag = 0;
        pps->pps_luma_beta_offset_div2 = 0;
        pps->pps_luma_tc_offset_div2 = 0;
        pps->pps_cb_beta_offset_div2 = 0;
        pps->pps_cb_tc_offset_div2 = 0;
        pps->pps_cr_beta_offset_div2 = 0;
        pps->pps_cr_tc_offset_div2 = 0;
      }


      if( !pps->pps_no_pic_partition_flag ) {
      bool tmp_pps_rpl_info_in_ph_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_pps_rpl_info_in_ph_flag));
      pps->pps_rpl_info_in_ph_flag = tmp_pps_rpl_info_in_ph_flag;
      DLOG(INFO) << "## pps_rpl_info_in_ph_flag : " << ( tmp_pps_rpl_info_in_ph_flag ? "1" : "0");
      bool tmp_pps_sao_info_in_ph_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_pps_sao_info_in_ph_flag));
      pps->pps_sao_info_in_ph_flag = tmp_pps_sao_info_in_ph_flag;
      DLOG(INFO) << "## pps_sao_info_in_ph_flag : " << ( tmp_pps_sao_info_in_ph_flag ? "1" : "0");
      bool tmp_pps_alf_info_in_ph_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_pps_alf_info_in_ph_flag));
      pps->pps_alf_info_in_ph_flag = tmp_pps_alf_info_in_ph_flag;
      DLOG(INFO) << "## pps_alf_info_in_ph_flag : " << ( tmp_pps_alf_info_in_ph_flag ? "1" : "0");


        if( ( pps->pps_weighted_pred_flag || pps->pps_weighted_bipred_flag ) && pps->pps_rpl_info_in_ph_flag ){
          bool tmp_pps_wp_info_in_ph_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_pps_wp_info_in_ph_flag));
          pps->pps_wp_info_in_ph_flag = tmp_pps_wp_info_in_ph_flag;
          DLOG(INFO) << "## pps_wp_info_in_ph_flag : " << ( tmp_pps_wp_info_in_ph_flag ? "1" : "0");
         }
        bool tmp_pps_qp_delta_info_in_ph_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_pps_qp_delta_info_in_ph_flag));
        pps->pps_qp_delta_info_in_ph_flag = tmp_pps_qp_delta_info_in_ph_flag;
        DLOG(INFO) << "## pps_qp_delta_info_in_ph_flag : " << ( tmp_pps_qp_delta_info_in_ph_flag ? "1" : "0");
      }
      bool tmp_pps_slice_header_extension_present_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_pps_slice_header_extension_present_flag));
      pps->pps_slice_header_extension_present_flag = tmp_pps_slice_header_extension_present_flag;
      DLOG(INFO) << "## pps_slice_header_extension_present_flag : " << ( tmp_pps_slice_header_extension_present_flag ? "1" : "0");
      bool tmp_pps_extension_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_pps_extension_flag));
      pps->pps_extension_flag = tmp_pps_extension_flag;
      DLOG(INFO) << "## pps_extension_flag : " << ( tmp_pps_extension_flag ? "1" : "0");

      if( pps->pps_extension_flag ) {
          while( br->more_rbsp_data() ) {
              bool extension_data_flag;
              TRUE_OR_RETURN(br->ReadBool(&extension_data_flag));
              pps->pps_extension_data_flags = extension_data_flag;
              DLOG(INFO) << "## pps_extension_data_flag : " << ( extension_data_flag ? "1" : "0");
          }
      }
      OK_OR_RETURN(ByteAlignment(br));


    }
  }
} else {

  DLOG(INFO) << "## DebuG pps_pic_width_in_luma_samples :" << pps->pps_pic_width_in_luma_samples;
  DLOG(INFO) << "## DebuG pps_pic_height_in_luma_samples : " << pps->pps_pic_height_in_luma_samples;
  DLOG(INFO) << "## DebuG CtbSizeY FROM PPS : " << pps->CtbSizeY;
  DLOG(INFO) << "## DebuG CtbSizeY FROM SPS : " << sps->CtbSizeY;

  int local_CtbSizeY = std::max(pps->CtbSizeY, sps->CtbSizeY);

  int PicWidthInCtbsY = 0;   
  int PicHeightInCtbsY = 0;
  if (local_CtbSizeY > 0) {
      PicWidthInCtbsY = ceil( pps->pps_pic_width_in_luma_samples /  local_CtbSizeY );
      PicHeightInCtbsY = ceil( pps->pps_pic_height_in_luma_samples /  local_CtbSizeY );
  } else {
      DLOG(ERROR) << "## Error CtbSizeY is zero in both SPS and PPS, cannot compute PicWidthInCtbsY and PicHeightInCtbsY";
      return kInvalidStream;
  }


  pps->pps_num_exp_tile_columns_minus1 = 0;
  pps->pps_tile_column_width_minus1.push_back(PicWidthInCtbsY - 1);
  pps->pps_num_exp_tile_rows_minus1 = 0;
  pps->pps_tile_row_height_minus1.push_back(PicHeightInCtbsY - 1);
  pps->NumTileColumns = 1;
  pps->NumTileRows = 1;
  pps->NumTilesInPic = 1;

}


   //DisplayH266PPS(*pps);

  // This will replace any existing PPS instance.
  *pps_id = pps->pic_parameter_set_id;

  DLOG(INFO) << "## Replace Old PPs instance by New instance with this id : " <<  pps->pic_parameter_set_id;

  active_ppses_.emplace(*pps_id, std::move(pps));

  if (!HasPps(*pps_id)){
    DLOG(ERROR) << "# Back Up Pps Instance no Work";
  }

  //active_ppses_[*pps_id] = std::move(pps);

  return kOk;
}


H266Parser::Result H266Parser::ParseSps(const Nalu& nalu, int* sps_id) {
  DCHECK_EQ(Nalu::H266_SPS_NUT, nalu.type());
  LOG(INFO) << "Parsing H.266 SPS NALU";
  //seq_parameter_set_rbsp( ) 7.3.2.4

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *sps_id = -1;
  std::unique_ptr<H266Sps> sps(new H266Sps);



  TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_seq_parameter_set_id));
  DLOG(INFO) << "## sps_seq_parameter_set_id : " << sps->sps_seq_parameter_set_id;
  TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_video_parameter_set_id));
  DLOG(INFO) << "## sps_video_parameter_set_id : " << sps->sps_video_parameter_set_id;



  TRUE_OR_RETURN(br->ReadBits(3, &sps->max_sublayers_minus1));
  DLOG(INFO) << "## max_sublayers_minus1 : " << sps->max_sublayers_minus1;

  sps->max_sublayers = sps->max_sublayers_minus1 + 1;
  TRUE_OR_RETURN(br->ReadBits(2, &sps->sps_chroma_format_idc));
  DLOG(INFO) << "## sps_chroma_format_idc : " << sps->sps_chroma_format_idc;


  TRUE_OR_RETURN(br->ReadBits(2, &sps->sps_log2_ctu_size_minus5));
  DLOG(INFO) << "## sps_log2_ctu_size_minus5 : " << sps->sps_log2_ctu_size_minus5;

  // define CtbSizeY
  int CtbLog2SizeY = sps->sps_log2_ctu_size_minus5 + 5;
  int CtbSizeY = 1 << CtbLog2SizeY;
  DLOG(INFO) << "## CtbSizeY : " << CtbSizeY << "CtbLog2SizeY : " << CtbLog2SizeY;
  sps->CtbSizeY = CtbSizeY;


  TRUE_OR_RETURN(br->ReadBool(&sps->sps_ptl_dpb_hrd_params_present_flag));
  DLOG(INFO) << "## sps_ptl_dpb_hrd_params_present_flag : " <<  ( sps->sps_ptl_dpb_hrd_params_present_flag ? "1" : "0");
  if( sps->sps_ptl_dpb_hrd_params_present_flag) {
    ParseProfileTierLevel(true, sps->max_sublayers_minus1, br, &sps->sps_profile_level);
  }
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_gdr_enabled_flag));
  DLOG(INFO) << "## sps_gdr_enabled_flag : " << ( sps->sps_gdr_enabled_flag ? "1" : "0");
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_ref_pic_resampling_enabled_flag));
  DLOG(INFO) << "## sps_ref_pic_resampling_enabled_flag : " << ( sps->sps_ref_pic_resampling_enabled_flag ? "1" : "0");
  if( sps->sps_ref_pic_resampling_enabled_flag) {
    TRUE_OR_RETURN(br->ReadBool(&sps->sps_res_change_in_clvs_allowed_flag));
    DLOG(INFO) << "## ssps_ref_pic_resampling_enabled_flag : " << ( sps->sps_ref_pic_resampling_enabled_flag ? "1" : "0");
 }
  TRUE_OR_RETURN(br->ReadUE(&sps->sps_pic_width_max_in_luma_samples));
  DLOG(INFO) << "## sps_pic_width_max_in_luma_samples : " << sps->sps_pic_width_max_in_luma_samples;

  TRUE_OR_RETURN(br->ReadUE(&sps->sps_pic_height_max_in_luma_samples));
  DLOG(INFO) << "## sps_pic_width_max_in_luma_samples : " << sps->sps_pic_height_max_in_luma_samples;

  TRUE_OR_RETURN(br->ReadBool(&sps->sps_conformance_window_flag));
  DLOG(INFO) << "## sps_conformance_window_flag : " << ( sps->sps_conformance_window_flag ? "1" : "0");


  if (sps->sps_conformance_window_flag) {
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_left_offset));
    DLOG(INFO) << "## sps_conf_win_left_offset  : " << sps->sps_conf_win_left_offset;
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_right_offset));
    DLOG(INFO) << "##  : sps_conf_win_right_offset " << sps->sps_conf_win_right_offset;
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_top_offset));
    DLOG(INFO) << "##  : sps_conf_win_top_offset" << sps->sps_conf_win_top_offset;
    TRUE_OR_RETURN(br->ReadUE(&sps->sps_conf_win_bottom_offset));
    DLOG(INFO) << "## sps_conf_win_bottom_offset : " << sps->sps_conf_win_bottom_offset;
  }
  TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_info_present_flag));
  DLOG(INFO) << "## sps_subpic_info_present_flag : " << (sps->sps_subpic_info_present_flag  ? "1" : "0");
  if(sps->sps_subpic_info_present_flag) {
        TRUE_OR_RETURN(br->ReadUE(&sps->sps_num_subpics_minus1));
        DLOG(INFO) << "## sps_num_subpics_minus1 : " << sps->sps_num_subpics_minus1;

        if(sps->sps_num_subpics_minus1 > 0) {
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_independent_subpics_flag));
          DLOG(INFO) << "## sps_independent_subpics_flag : " << (sps->sps_independent_subpics_flag ? "1" : "0");
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_subpic_same_size_flag));
          DLOG(INFO) << "## sps_subpic_same_size_flag : " << ( sps->sps_subpic_same_size_flag ? "1" : "0");
        }

        int tmpWidthVal = ((sps->sps_pic_width_max_in_luma_samples + sps->CtbSizeY -1 ) / sps->CtbSizeY);
        int tmpHeightVal = (( sps->sps_pic_height_max_in_luma_samples + sps->CtbSizeY -1 ) / sps->CtbSizeY);
        // todo recheck this section

       int bitWidth = ceil(log2(tmpWidthVal));
       int bitHeight = ceil(log2(tmpHeightVal));

        if(sps->sps_num_subpics_minus1 > 0){
          int numSubpicCols = 1; //same size sub pic  need to evaluate  later 
          int num_subpics = sps->sps_num_subpics_minus1 + 1;
          sps->sps_subpic_ctu_top_left_x.resize(num_subpics);
          sps->sps_subpic_ctu_top_left_y.resize(num_subpics);
          sps->sps_subpic_width_minus1.resize(num_subpics);
          sps->sps_subpic_height_minus1.resize(num_subpics);

          sps->sps_subpic_ctu_top_left_x[0] = 0;
          sps->sps_subpic_ctu_top_left_y[0] = 0;




          #if 0
            for(int i=0; i<= sps->sps_num_subpics_minus1 ; i++) {
              if(!sps->sps_subpic_same_size_flag && i>0) {
                /* // define CtbSizeY
                int CtbLog2SizeY = sps->sps_log2_ctu_size_minus5 + 5;
                int CtbSizeY = 1 << CtbLog2SizeY;
                DLOG(INFO) << "## CtbSizeY : " << CtbSizeY << "CtbLog2SizeY : " << CtbLog2SizeY;
                sps->CtbSizeY = CtbSizeY; */


           /*      int tmpWidthVal = ((sps->sps_pic_width_max_in_luma_samples + sps->CtbSizeY-1 ) / sps->CtbSizeY);
                int tmpHeightVal = (( sps->sps_pic_height_max_in_luma_samples + sps->CtbSizeY-1 ) / sps->CtbSizeY);
                // todo recheck this section
                int bit_read_WidthVal = ceil(log2(tmpWidthVal));
                int bit_read_HeightVal = ceil(log2(tmpHeightVal));
 */
                u_int tmp_sps_subpic_ctu_top_left_x = 0;
                if(i>0 && sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY) {
                  TRUE_OR_RETURN(br->ReadBits(bitWidth,&tmp_sps_subpic_ctu_top_left_x));  // u(v)  NOT SURE
                  sps->sps_subpic_ctu_top_left_x.push_back(tmp_sps_subpic_ctu_top_left_x);
                  DLOG(INFO) << "## sps_subpic_ctu_top_left_x : " << tmp_sps_subpic_ctu_top_left_x;
                }
                int tmp_sps_subpic_ctu_top_left_y = 0;
                if( i > 0 && sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY ){
                  TRUE_OR_RETURN(br->ReadBits(bitHeight,&tmp_sps_subpic_ctu_top_left_y));  // u(v)  NOT SURE
                  sps->sps_subpic_ctu_top_left_y.push_back(tmp_sps_subpic_ctu_top_left_y);
                  DLOG(INFO) << "## sps_subpic_ctu_top_left_y : " << tmp_sps_subpic_ctu_top_left_y;
                } else {
                  sps->sps_subpic_ctu_top_left_y.push_back(0);
                }

                int tmp_sps_subpic_width_minus1 = 0;
                if( i < sps->sps_num_subpics_minus1 && sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY ){
                  TRUE_OR_RETURN(br->ReadBits(bitWidth,&tmp_sps_subpic_width_minus1));  // u(v)  NOT SURE
                  sps->sps_subpic_width_minus1.push_back(tmp_sps_subpic_width_minus1);
                  DLOG(INFO) << "## sps_subpic_width_minus1 : " << tmp_sps_subpic_width_minus1;
                } else {
                  sps->sps_subpic_width_minus1.push_back( tmpWidthVal - sps->sps_subpic_ctu_top_left_x[i] - 1 );
                }

                int tmp_sps_subpic_height_minus1 = 0;
                if( i < sps->sps_num_subpics_minus1 && sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY ){
                  //sps_subpic_height_minus1[
                  TRUE_OR_RETURN(br->ReadBits(bitHeight,&tmp_sps_subpic_height_minus1));  // u(v)  NOT SURE
                  sps->sps_subpic_ctu_top_left_y.push_back(tmp_sps_subpic_height_minus1);
                  DLOG(INFO) << "## sps_subpic_height_minus1 : " << tmp_sps_subpic_height_minus1;
                } else {
                  sps->sps_subpic_height_minus1.push_back( tmpHeightVal - sps->sps_subpic_ctu_top_left_y[i] - 1 );
                }

              }
              if( !sps->sps_independent_subpics_flag) {
                bool tmp_sps_subpic_treated_as_pic_flag;
                bool tmp_sps_loop_filter_across_subpic_enabled_flag;
                sps->sps_subpic_treated_as_pic_flag.resize(num_subpics);
                sps->sps_loop_filter_across_subpic_enabled_flag.resize(num_subpics);

              TRUE_OR_RETURN(br->ReadBool(&tmp_sps_subpic_treated_as_pic_flag));
              sps->sps_subpic_treated_as_pic_flag.push_back(tmp_sps_subpic_treated_as_pic_flag);
              DLOG(INFO) << "## sps_subpic_treated_as_pic_flag : " << ( tmp_sps_subpic_treated_as_pic_flag ? "1" : "0");
              TRUE_OR_RETURN(br->ReadBool(&tmp_sps_loop_filter_across_subpic_enabled_flag));
              sps->sps_loop_filter_across_subpic_enabled_flag.push_back(tmp_sps_loop_filter_across_subpic_enabled_flag);
              DLOG(INFO) << "## sps_loop_filter_across_subpic_enabled_flag : " << ( tmp_sps_loop_filter_across_subpic_enabled_flag ? "1" : "0");
              }
            } //for
            #else
            //int numSubpicCols = 1;

            for (int i = 0; i <= sps->sps_num_subpics_minus1; i++) {

                  if (!sps->sps_subpic_same_size_flag && i > 0) {
                      // Lire les positions et dimensions pour chaque sous-image
                      if (sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY) {
                          TRUE_OR_RETURN(br->ReadBits(bitWidth, &sps->sps_subpic_ctu_top_left_x[i]));
                      } else {
                          sps->sps_subpic_ctu_top_left_x[i] = 0;
                      }
                      
                      if (sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY) {
                          TRUE_OR_RETURN(br->ReadBits(bitHeight, &sps->sps_subpic_ctu_top_left_y[i]));
                      } else {
                          sps->sps_subpic_ctu_top_left_y[i] = 0;
                      }
                      
                      if (i < sps->sps_num_subpics_minus1 && sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY) {
                          TRUE_OR_RETURN(br->ReadBits(bitWidth, &sps->sps_subpic_width_minus1[i]));
                      } else {
                          // Dernière sous-image : calculer la largeur restante
                          sps->sps_subpic_width_minus1[i] = tmpWidthVal - sps->sps_subpic_ctu_top_left_x[i] - 1;
                      }
                      
                      if (i < sps->sps_num_subpics_minus1 && sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY) {
                          TRUE_OR_RETURN(br->ReadBits(bitHeight, &sps->sps_subpic_height_minus1[i]));
                      } else {
                          // Dernière sous-image : calculer la hauteur restante
                          sps->sps_subpic_height_minus1[i] = tmpHeightVal - sps->sps_subpic_ctu_top_left_y[i] - 1;
                      }
                  } else if (sps->sps_subpic_same_size_flag) {
                      if (i == 0) {
                          // Pour la première sous-image, lire les valeurs
                          if (sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY) {
                              TRUE_OR_RETURN(br->ReadBits(bitWidth, &sps->sps_subpic_ctu_top_left_x[0]));
                              TRUE_OR_RETURN(br->ReadBits(bitWidth, &sps->sps_subpic_width_minus1[0]));
                          } else {
                              sps->sps_subpic_ctu_top_left_x[0] = 0;
                              sps->sps_subpic_width_minus1[0] = tmpWidthVal - 1;
                          }
                          
                          if (sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY) {
                              TRUE_OR_RETURN(br->ReadBits(bitHeight, &sps->sps_subpic_ctu_top_left_y[0]));
                              TRUE_OR_RETURN(br->ReadBits(bitHeight, &sps->sps_subpic_height_minus1[0]));
                          } else {
                              sps->sps_subpic_ctu_top_left_y[0] = 0;
                              sps->sps_subpic_height_minus1[0] = tmpHeightVal - 1;
                          }
                          
                          // Calculer numSubpicCols pour les sous-images de même taille
                          numSubpicCols = tmpWidthVal / (sps->sps_subpic_width_minus1[0] + 1);
                      } else {
                          // Pour les sous-images suivantes, inférer les valeurs
                          sps->sps_subpic_ctu_top_left_x[i] = (i % numSubpicCols) * (sps->sps_subpic_width_minus1[0] + 1);
                          sps->sps_subpic_ctu_top_left_y[i] = (i / numSubpicCols) * (sps->sps_subpic_height_minus1[0] + 1);
                          sps->sps_subpic_width_minus1[i] = sps->sps_subpic_width_minus1[0];
                          sps->sps_subpic_height_minus1[i] = sps->sps_subpic_height_minus1[0];
                      }
                  } else {
                      // i = 0 et sps_subpic_same_size_flag = 0
                      if (sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY) {
                          TRUE_OR_RETURN(br->ReadBits(bitWidth, &sps->sps_subpic_ctu_top_left_x[i]));
                      } else {
                          sps->sps_subpic_ctu_top_left_x[i] = 0;
                      }
                      
                      if (sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY) {
                          TRUE_OR_RETURN(br->ReadBits(bitHeight, &sps->sps_subpic_ctu_top_left_y[i]));
                      } else {
                          sps->sps_subpic_ctu_top_left_y[i] = 0;
                      }
                      
                      // Ces valeurs seront lues pour i < sps_num_subpics_minus1
                      if (i < sps->sps_num_subpics_minus1) {
                          if (sps->sps_pic_width_max_in_luma_samples > sps->CtbSizeY) {
                              TRUE_OR_RETURN(br->ReadBits(bitWidth, &sps->sps_subpic_width_minus1[i]));
                          }
                          if (sps->sps_pic_height_max_in_luma_samples > sps->CtbSizeY) {
                              TRUE_OR_RETURN(br->ReadBits(bitHeight, &sps->sps_subpic_height_minus1[i]));
                          }
                      } else {
                          // Dernière sous-image : calculer les dimensions restantes
                          sps->sps_subpic_width_minus1[i] = tmpWidthVal - sps->sps_subpic_ctu_top_left_x[i] - 1;
                          sps->sps_subpic_height_minus1[i] = tmpHeightVal - sps->sps_subpic_ctu_top_left_y[i] - 1;
                      }
                  }
                  
                  // Lire les flags pour chaque sous-image (si nécessaire)
                  if (!sps->sps_independent_subpics_flag) {
                      bool tmp_sps_subpic_treated_as_pic_flag = false;
                      bool tmp_sps_loop_filter_across_subpic_enabled_flag = false;
                      TRUE_OR_RETURN(br->ReadBool(&tmp_sps_subpic_treated_as_pic_flag));
                      sps->sps_subpic_treated_as_pic_flag.push_back(tmp_sps_subpic_treated_as_pic_flag);
                      TRUE_OR_RETURN(br->ReadBool(&tmp_sps_loop_filter_across_subpic_enabled_flag));
                      sps->sps_loop_filter_across_subpic_enabled_flag.push_back(tmp_sps_loop_filter_across_subpic_enabled_flag);
                  }
              }

            #endif



          }
        }//not sure
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_bitdepth_minus8));
          DLOG(INFO) << "## sps_bitdepth_minus8 : " << sps->sps_bitdepth_minus8;

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_entropy_coding_sync_enabled_flag));
          DLOG(INFO) << "## sps_entropy_coding_sync_enabled_flag : " << ( sps->sps_entropy_coding_sync_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_entry_point_offsets_present_flag));
          DLOG(INFO) << "## sps_entry_point_offsets_present_flag : " << ( sps->sps_entry_point_offsets_present_flag ? "1" : "0");


          TRUE_OR_RETURN(br->ReadBits(4,&sps->sps_log2_max_pic_order_cnt_lsb_minus4));
          DLOG(INFO) << "## sps_log2_max_pic_order_cnt_lsb_minus4 : " << sps->sps_log2_max_pic_order_cnt_lsb_minus4;

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_poc_msb_cycle_flag));
          DLOG(INFO) << "## sps_poc_msb_cycle_flag : " << ( sps->sps_poc_msb_cycle_flag ? "1" : "0");

          if(sps->sps_poc_msb_cycle_flag){
             TRUE_OR_RETURN(br->ReadUE(&sps->sps_poc_msb_cycle_len_minus1));
             DLOG(INFO) << "## sps_poc_msb_cycle_len_minus1 : " << sps->sps_poc_msb_cycle_len_minus1;
          }

          TRUE_OR_RETURN(br->ReadBits(2,&sps->sps_num_extra_ph_bytes));
          DLOG(INFO) << "## sps_num_extra_ph_bytes : " << sps->sps_num_extra_ph_bytes;


          int max_sps_num_extra_ph_bytes = sps->sps_num_extra_ph_bytes * 8;
          for( int i = 0; i < max_sps_num_extra_ph_bytes; i++ ){
            bool tmp_sps_extra_ph_bit_present_flag = 0;
            TRUE_OR_RETURN(br->ReadBool(&tmp_sps_extra_ph_bit_present_flag));
            sps->sps_extra_sh_bit_present_flag.push_back(tmp_sps_extra_ph_bit_present_flag);
            DLOG(INFO) << "## sps_extra_ph_bit_present_flag : " << ( tmp_sps_extra_ph_bit_present_flag ? "1" : "0");
          }
          TRUE_OR_RETURN(br->ReadBits(2,&sps->sps_num_extra_sh_bytes));
          DLOG(INFO) << "## sps_num_extra_sh_bytes : " << sps->sps_num_extra_sh_bytes;

          int max_sps_num_extra_sh_bytes = sps->sps_num_extra_sh_bytes * 8;
          for( int i = 0; i < max_sps_num_extra_sh_bytes; i++ ){
            bool tmp_sps_extra_sh_bit_present_flag = 0;
            TRUE_OR_RETURN(br->ReadBool(&tmp_sps_extra_sh_bit_present_flag));
            sps->sps_extra_sh_bit_present_flag.push_back(tmp_sps_extra_sh_bit_present_flag);
            DLOG(INFO) << "## sps_extra_sh_bit_present_flag : " << ( tmp_sps_extra_sh_bit_present_flag ? "1" : "0");
          }



          if( sps->sps_ptl_dpb_hrd_params_present_flag ) {
            if( sps->max_sublayers_minus1 > 0 ){
              TRUE_OR_RETURN(br->ReadBool(&sps->sps_sublayer_dpb_params_flag));
              DLOG(INFO) << "## sps_sublayer_dpb_params_flag : " << ( sps->sps_sublayer_dpb_params_flag ? "1" : "0");
            } //TODO
             //dpb_parameters( sps_max_sublayers_minus1, sps_sublayer_dpb_params_flag )
            if(!sps->sps_dpd.has_value()) {
                  sps->sps_dpd.emplace();
              }

              dpb_parameters( sps->max_sublayers_minus1, sps->sps_sublayer_dpb_params_flag ,
                          &sps->sps_dpd.value(),br);

          }
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_min_luma_coding_block_size_minus2));
          DLOG(INFO) << "## sps_log2_min_luma_coding_block_size_minus2 : " << sps->sps_log2_min_luma_coding_block_size_minus2;

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_partition_constraints_override_enabled_flag));
          DLOG(INFO) << "## sps_partition_constraints_override_enabled_flag : " << ( sps->sps_partition_constraints_override_enabled_flag ? "1" : "0");

          //Min( 6, CtbLog2SizeY ) − MinCbLog2SizeY
          int ctb_log2_size_y = sps->sps_log2_ctu_size_minus5 + 5;
          sps->CtbLog2SizeY = 1 << ctb_log2_size_y;
          sps->MinCbLog2SizeY = sps->sps_log2_min_luma_coding_block_size_minus2 + 2;

          int tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma = 0;

          //int max_tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma = std::min(6,sps->CtbLog2SizeY) - sps->MinCbLog2SizeY;

          TRUE_OR_RETURN(br->ReadUE(&tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma));
          sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma = tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma;
          DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_intra_slice_luma : " << sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma;



         /*  if ( tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma > 0 && tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma < max_tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma ){
            sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma = tmp_sps_log2_diff_min_qt_min_cb_intra_slice_luma;
            DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_intra_slice_luma : " << sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma;
          } else {
            DLOG(INFO) << "## Error sps_log2_diff_min_qt_min_cb_intra_slice_luma : " << sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma;
          } */

          TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_intra_slice_luma));
          DLOG(INFO) << "## sps_max_mtt_hierarchy_depth_intra_slice_luma : " << sps->sps_max_mtt_hierarchy_depth_intra_slice_luma;

          if( sps->sps_max_mtt_hierarchy_depth_intra_slice_luma != 0 ) {

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma));
            DLOG(INFO) << "## sps_log2_diff_max_bt_min_qt_intra_slice_luma : " << sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma;
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma));
            DLOG(INFO) << "## sps_log2_diff_max_tt_min_qt_intra_slice_luma : " << sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma;
          }
          if( sps->sps_chroma_format_idc != 0 ){
            bool tmp_sps_qtbtt_dual_tree_intra_flag = false;
            TRUE_OR_RETURN(br->ReadBool(&tmp_sps_qtbtt_dual_tree_intra_flag));
            sps->sps_qtbtt_dual_tree_intra_flag = tmp_sps_qtbtt_dual_tree_intra_flag;

            DLOG(INFO) << "## sps_qtbtt_dual_tree_intra_flag : " << ( sps->sps_qtbtt_dual_tree_intra_flag ? "1" : "0");
          }
          if( sps->sps_qtbtt_dual_tree_intra_flag ) {

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma));
            DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_intra_slice_chroma : " << sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma;

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma));
            DLOG(INFO) << "## sps_max_mtt_hierarchy_depth_intra_slice_chroma : " << sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma;

            if( sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma != 0 ) {

                TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma));
                DLOG(INFO) << "## sps_log2_diff_max_bt_min_qt_intra_slice_chroma : " << sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma;

                TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma));
                DLOG(INFO) << "## sps_log2_diff_max_tt_min_qt_intra_slice_chroma : " << sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma;
            }
          }
          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_min_qt_min_cb_inter_slice));
          DLOG(INFO) << "## sps_log2_diff_min_qt_min_cb_inter_slice : " << sps->sps_log2_diff_min_qt_min_cb_inter_slice;

          TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_mtt_hierarchy_depth_inter_slice));
          DLOG(INFO) << "## sps_max_mtt_hierarchy_depth_inter_slice : " << sps->sps_max_mtt_hierarchy_depth_inter_slice;

          if( sps->sps_max_mtt_hierarchy_depth_inter_slice != 0 ) {
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_bt_min_qt_inter_slice));
            DLOG(INFO) << "## sps_log2_diff_max_bt_min_qt_inter_slice : " << sps->sps_log2_diff_max_bt_min_qt_inter_slice;

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_diff_max_tt_min_qt_inter_slice));
            DLOG(INFO) << "## sps_log2_diff_max_tt_min_qt_inter_slice : " << sps->sps_log2_diff_max_tt_min_qt_inter_slice;
          }

          // Derive CTB size from SPS and use it instead of undefined 'pps'.
          //int ctb_log2_size_y = sps->sps_log2_ctu_size_minus5 + 5;
          //int ctb_size_y = 1 << ctb_log2_size_y;
          //if (ctb_size_y > 32) {
          if (sps->CtbSizeY > 32) {


            TRUE_OR_RETURN(br->ReadBool(&sps->sps_max_luma_transform_size_64_flag));
            DLOG(INFO) << "## sps_max_luma_transform_size_64_flag : " << ( sps->sps_max_luma_transform_size_64_flag ? "1" : "0");
          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_transform_skip_enabled_flag));
          DLOG(INFO) << "## sps_transform_skip_enabled_flag : " << ( sps->sps_transform_skip_enabled_flag ? "1" : "0");

          if( sps->sps_transform_skip_enabled_flag ) {

            TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_transform_skip_max_size_minus2));
            DLOG(INFO) << "## sps_log2_transform_skip_max_size_minus2 : " << sps->sps_log2_transform_skip_max_size_minus2;

            TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdpcm_enabled_flag));
            DLOG(INFO) << "## sps_bdpcm_enabled_flag : " << ( sps->sps_bdpcm_enabled_flag ? "1" : "0");
          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_mts_enabled_flag));
          DLOG(INFO) << "## sps_mts_enabled_flag : " << ( sps->sps_mts_enabled_flag ? "1" : "0");

          if( sps->sps_mts_enabled_flag ) {
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_mts_intra_enabled_flag));
            DLOG(INFO) << "## sps_explicit_mts_intra_enabled_flag : " << ( sps->sps_explicit_mts_intra_enabled_flag ? "1" : "0");

            TRUE_OR_RETURN(br->ReadBool(&sps->sps_explicit_mts_inter_enabled_flag));
            DLOG(INFO) << "## sps_explicit_mts_inter_enabled_flag : " << ( sps->sps_explicit_mts_inter_enabled_flag ? "1" : "0");
          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_lfnst_enabled_flag));
          DLOG(INFO) << "## sps_lfnst_enabled_flag : " << ( sps->sps_lfnst_enabled_flag ? "1" : "0");

          if( sps->sps_chroma_format_idc != 0 ) {
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_joint_cbcr_enabled_flag));
            DLOG(INFO) << "## sps_joint_cbcr_enabled_flag : " << ( sps->sps_joint_cbcr_enabled_flag ? "1" : "0");

            TRUE_OR_RETURN(br->ReadBool(&sps->sps_same_qp_table_for_chroma_flag));
            DLOG(INFO) << "## sps_same_qp_table_for_chroma_flag : " << ( sps->sps_same_qp_table_for_chroma_flag ? "1" : "0");

            int numQpTables = sps->sps_same_qp_table_for_chroma_flag ? 1 : ( sps->sps_joint_cbcr_enabled_flag ? 3 : 2 );


            sps->sps_qp_table_start_minus26.reserve(numQpTables);
            sps->sps_num_points_in_qp_table_minus1.reserve(numQpTables);

            for( int i = 0; i < numQpTables; i++ ) {
              int tmp_sps_qp_table_start_minus26 = 0;
              int tmp_sps_num_points_in_qp_table_minus1 = 0;

              TRUE_OR_RETURN(br->ReadSE(&tmp_sps_qp_table_start_minus26));
              DLOG(INFO) << "## sps_qp_table_start_minus26 : " << tmp_sps_qp_table_start_minus26;
              sps->sps_qp_table_start_minus26.push_back(tmp_sps_qp_table_start_minus26);

              TRUE_OR_RETURN(br->ReadUE(&tmp_sps_num_points_in_qp_table_minus1));
              sps->sps_num_points_in_qp_table_minus1.push_back(tmp_sps_num_points_in_qp_table_minus1);
              DLOG(INFO) << "## sps_num_points_in_qp_table_minus1 : " << tmp_sps_num_points_in_qp_table_minus1;

              int tmp_sps_delta_qp_in_val_minus1 = 0;
              int tmp_sps_delta_qp_diff_val = 0;

              if (sps->sps_delta_qp_in_val_minus1.size() <= static_cast<size_t>(i)) {
                  sps->sps_delta_qp_in_val_minus1.resize(i + 1);
                  sps->sps_delta_qp_diff_val.resize(i + 1);
              }
              if (sps->sps_delta_qp_in_val_minus1[i].size() <= static_cast<size_t>(tmp_sps_num_points_in_qp_table_minus1)) {
                  sps->sps_delta_qp_in_val_minus1[i].resize(tmp_sps_num_points_in_qp_table_minus1 + 1);
                  sps->sps_delta_qp_diff_val[i].resize(tmp_sps_num_points_in_qp_table_minus1 + 1);
              }

              //for( int j = 0; j <= sps->sps_num_points_in_qp_table_minus1[ i ]; j++ ) {
              for( size_t j = 0; j <= static_cast<size_t>(sps->sps_num_points_in_qp_table_minus1[ i ]); j++ ) {

                TRUE_OR_RETURN(br->ReadUE(&tmp_sps_delta_qp_in_val_minus1));
                DLOG(INFO) << "## sps_delta_qp_in_val_minus1 : " << tmp_sps_delta_qp_in_val_minus1;

                TRUE_OR_RETURN(br->ReadUE(&tmp_sps_delta_qp_diff_val));
                DLOG(INFO) << "## sps_delta_qp_diff_val : " << tmp_sps_delta_qp_diff_val;

                sps->sps_delta_qp_in_val_minus1[i][j].push_back(tmp_sps_delta_qp_in_val_minus1);
                sps->sps_delta_qp_diff_val[i][j].push_back(tmp_sps_delta_qp_diff_val);
              }
            }
          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_sao_enabled_flag));
          DLOG(INFO) << "## sps_sao_enabled_flag : " << ( sps->sps_sao_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_alf_enabled_flag));
          DLOG(INFO) << "## sps_alf_enabled_flag : " << ( sps->sps_alf_enabled_flag ? "1" : "0");

          if( sps->sps_alf_enabled_flag && sps->sps_chroma_format_idc != 0 ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_ccalf_enabled_flag));
            DLOG(INFO) << "## sps_ccalf_enabled_flag : " << ( sps->sps_ccalf_enabled_flag ? "1" : "0");
          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_lmcs_enabled_flag));
          DLOG(INFO) << "## sps_lmcs_enabled_flag : " << ( sps->sps_lmcs_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_weighted_pred_flag));
          DLOG(INFO) << "## sps_weighted_pred_flag : " << ( sps->sps_weighted_pred_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_weighted_bipred_flag));
          DLOG(INFO) << "## sps_weighted_bipred_flag : " << ( sps->sps_weighted_bipred_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_long_term_ref_pics_flag));
          DLOG(INFO) << "## sps_long_term_ref_pics_flag : " << ( sps->sps_long_term_ref_pics_flag ? "1" : "0");

          if( sps->sps_video_parameter_set_id > 0 ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_inter_layer_prediction_enabled_flag));
            DLOG(INFO) << "## sps_inter_layer_prediction_enabled_flag : " << ( sps->sps_inter_layer_prediction_enabled_flag ? "1" : "0");
          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_idr_rpl_present_flag));
          DLOG(INFO) << "## sps_idr_rpl_present_flag : " << ( sps->sps_idr_rpl_present_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_long_term_ref_pics_flag));
          DLOG(INFO) << "## sps_long_term_ref_pics_flag : " << ( sps->sps_long_term_ref_pics_flag ? "1" : "0");

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
          DLOG(INFO) << "## sps_ref_wraparound_enabled_flag : " << ( sps->sps_ref_wraparound_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_temporal_mvp_enabled_flag));
          DLOG(INFO) << "## sps_temporal_mvp_enabled_flag : " << ( sps->sps_temporal_mvp_enabled_flag ? "1" : "0");

          if( sps->sps_temporal_mvp_enabled_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_sbtmvp_enabled_flag));
            DLOG(INFO) << "## sps_sbtmvp_enabled_flag : " << ( sps->sps_sbtmvp_enabled_flag ? "1" : "0");
          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_amvr_enabled_flag));
          DLOG(INFO) << "## sps_amvr_enabled_flag : " << ( sps->sps_amvr_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdof_enabled_flag));
          DLOG(INFO) << "## sps_bdof_enabled_flag : " << ( sps->sps_bdof_enabled_flag ? "1" : "0");

          if(sps->sps_bdof_enabled_flag){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_bdof_control_present_in_ph_flag));
            DLOG(INFO) << "## sps_bdof_control_present_in_ph_flag : " << ( sps->sps_bdof_control_present_in_ph_flag ? "1" : "0");
          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_smvd_enabled_flag));
          DLOG(INFO) << "## sps_smvd_enabled_flag : " << ( sps->sps_smvd_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_dmvr_enabled_flag));
          DLOG(INFO) << "## sps_dmvr_enabled_flag : " << ( sps->sps_dmvr_enabled_flag ? "1" : "0");
          if(sps->sps_dmvr_enabled_flag){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_dmvr_control_present_in_ph_flag));
            DLOG(INFO) << "## sps_dmvr_control_present_in_ph_flag : " << ( sps->sps_dmvr_control_present_in_ph_flag ? "1" : "0");
          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_mmvd_enabled_flag));
          DLOG(INFO) << "## sps_mmvd_enabled_flag : " << ( sps->sps_mmvd_enabled_flag ? "1" : "0");

          if(sps->sps_mmvd_enabled_flag){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_mmvd_fullpel_only_enabled_flag));
            DLOG(INFO) << "## sps_mmvd_fullpel_only_enabled_flag : " << ( sps->sps_mmvd_fullpel_only_enabled_flag ? "1" : "0");
          }

          TRUE_OR_RETURN(br->ReadUE(&sps->sps_six_minus_max_num_merge_cand));
          DLOG(INFO) << "## sps_six_minus_max_num_merge_cand : " << sps->sps_six_minus_max_num_merge_cand;

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_sbt_enabled_flag));
          DLOG(INFO) << "## sps_sbt_enabled_flag : " << (sps->sps_sbt_enabled_flag  ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_affine_enabled_flag));
          DLOG(INFO) << "## sps_affine_enabled_flag : " << ( sps->sps_affine_enabled_flag ? "1" : "0");

          if (sps->sps_affine_enabled_flag){
              TRUE_OR_RETURN(br->ReadUE(&sps->sps_five_minus_max_num_subblock_merge_cand));
              DLOG(INFO) << "## sps_five_minus_max_num_subblock_merge_cand" << sps->sps_five_minus_max_num_subblock_merge_cand;

              TRUE_OR_RETURN(br->ReadBool(&sps->sps_6param_affine_enabled_flag));
              DLOG(INFO) << "## sps_6param_affine_enabled_flag : " << ( sps->sps_6param_affine_enabled_flag ? "1" : "0");
            //}

            if(sps->sps_amvr_enabled_flag){

              TRUE_OR_RETURN(br->ReadBool(&sps->sps_affine_amvr_enabled_flag));
              DLOG(INFO) << "## sps_affine_amvr_enabled_flag : " << (sps->sps_affine_amvr_enabled_flag ? "1" : "0");
            }

            TRUE_OR_RETURN(br->ReadBool(&sps->sps_affine_prof_enabled_flag));
            DLOG(INFO) << "## sps_affine_prof_enabled_flag : " << ( sps->sps_affine_prof_enabled_flag ? "1" : "0");

            if(sps->sps_affine_prof_enabled_flag){
              TRUE_OR_RETURN(br->ReadBool(&sps->sps_prof_control_present_in_ph_flag));
              DLOG(INFO) << "## sps_prof_control_present_in_ph_flag : " << ( sps->sps_prof_control_present_in_ph_flag ? "1" : "0");
            }
          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_bcw_enabled_flag));
          DLOG(INFO) << "## sps_bcw_enabled_flag : " << ( sps->sps_bcw_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_ciip_enabled_flag));
          DLOG(INFO) << "## sps_ciip_enabled_flag : " << ( sps->sps_ciip_enabled_flag ? "1" : "0");

          int MaxNumMergeCand = 6 - sps->sps_six_minus_max_num_merge_cand;
          if (MaxNumMergeCand >= 2){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_gpm_enabled_flag));
            DLOG(INFO) << "## sps_gpm_enabled_flag : " << ( sps->sps_gpm_enabled_flag ? "1" : "0");

            if( sps->sps_gpm_enabled_flag && MaxNumMergeCand >= 3 ){
              TRUE_OR_RETURN(br->ReadUE(&sps->sps_max_num_merge_cand_minus_max_num_gpm_cand));
            }
          }

          TRUE_OR_RETURN(br->ReadUE(&sps->sps_log2_parallel_merge_level_minus2));

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_isp_enabled_flag));
          DLOG(INFO) << "## sps_isp_enabled_flag : " << ( sps->sps_isp_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_mrl_enabled_flag));
          DLOG(INFO) << "## sps_mrl_enabled_flag : " << ( sps->sps_mrl_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_mip_enabled_flag));
          DLOG(INFO) << "## sps_mip_enabled_flag : " << ( sps->sps_mip_enabled_flag ? "1" : "0");

          if( sps->sps_chroma_format_idc != 0 ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_cclm_enabled_flag));
           DLOG(INFO) << "## sps_cclm_enabled_flag : " << ( sps->sps_cclm_enabled_flag ? "1" : "0");
          }

         // DLOG(INFO) << "##  : " << (  ? "1" : "0");



          if( sps->sps_chroma_format_idc == 1 ) {
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_chroma_horizontal_collocated_flag));
          DLOG(INFO) << "## sps_chroma_horizontal_collocated_flag  : " << ( sps->sps_chroma_horizontal_collocated_flag ? "1" : "0");

            TRUE_OR_RETURN(br->ReadBool(&sps->sps_chroma_vertical_collocated_flag));
           DLOG(INFO) << "## sps_chroma_vertical_collocated_flag : " << (sps->sps_chroma_vertical_collocated_flag ? "1" : "0");

          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_palette_enabled_flag));
          DLOG(INFO) << "##  sps_palette_enabled_flag: " << (sps->sps_palette_enabled_flag  ? "1" : "0");

          if( sps->sps_chroma_format_idc == 3 && !sps->sps_max_luma_transform_size_64_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_act_enabled_flag));
            DLOG(INFO) << "## sps_act_enabled_flag : " << (sps->sps_act_enabled_flag  ? "1" : "0");

          }


          if( sps->sps_transform_skip_enabled_flag || sps->sps_palette_enabled_flag ){
            TRUE_OR_RETURN(br->ReadUE(&sps->sps_min_qp_prime_ts));
          }

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_ibc_enabled_flag));
          DLOG(INFO) << "## sps_ibc_enabled_flag : " << ( sps->sps_ibc_enabled_flag ? "1" : "0");

          if(sps->sps_ibc_enabled_flag){
              TRUE_OR_RETURN(br->ReadUE(&sps->sps_six_minus_max_num_ibc_merge_cand));
          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_ladf_enabled_flag));
          DLOG(INFO) << "## sps_ladf_enabled_flag : " << ( sps->sps_ladf_enabled_flag ? "1" : "0");

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
          DLOG(INFO) << "## sps_explicit_scaling_list_enabled_flag : " << ( sps->sps_explicit_scaling_list_enabled_flag ? "1" : "0");

          if( sps->sps_lfnst_enabled_flag && sps->sps_explicit_scaling_list_enabled_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_scaling_matrix_for_lfnst_disabled_flag));
            DLOG(INFO) << "## sps_scaling_matrix_for_lfnst_disabled_flag : " << ( sps->sps_scaling_matrix_for_lfnst_disabled_flag ? "1" : "0");

          }
          if( sps->sps_act_enabled_flag && sps->sps_explicit_scaling_list_enabled_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_scaling_matrix_for_alternative_colour_space_disabled_flag));
            DLOG(INFO) << "## sps_scaling_matrix_for_alternative_colour_space_disabled_flag : " << ( sps->sps_scaling_matrix_for_alternative_colour_space_disabled_flag ? "1" : "0");

          }
          if( sps->sps_scaling_matrix_for_alternative_colour_space_disabled_flag ){
            TRUE_OR_RETURN(br->ReadBool(&sps->sps_scaling_matrix_designated_colour_space_flag));
            DLOG(INFO) << "## sps_scaling_matrix_designated_colour_space_flag : " << ( sps->sps_scaling_matrix_designated_colour_space_flag ? "1" : "0");

          }
          TRUE_OR_RETURN(br->ReadBool(&sps->sps_dep_quant_enabled_flag));
          DLOG(INFO) << "## sps_dep_quant_enabled_flag : " << ( sps->sps_dep_quant_enabled_flag ? "1" : "0");

          TRUE_OR_RETURN(br->ReadBool(&sps->sps_sign_data_hiding_enabled_flag));
          DLOG(INFO) << "## sps_sign_data_hiding_enabled_flag : " << ( sps->sps_sign_data_hiding_enabled_flag ? "1" : "0");


          TRUE_OR_RETURN(br->ReadBool(&sps->sps_virtual_boundaries_enabled_flag));
          DLOG(INFO) << "## sps_virtual_boundaries_enabled_flag : " << ( sps->sps_virtual_boundaries_enabled_flag ? "1" : "0");

          if(sps->sps_virtual_boundaries_enabled_flag){
              TRUE_OR_RETURN(br->ReadBool(&sps->sps_virtual_boundaries_present_flag));
              DLOG(INFO) << "## sps_virtual_boundaries_enabled_flag : " << ( sps->sps_virtual_boundaries_enabled_flag ? "1" : "0");

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
              DLOG(INFO) << "## sps_timing_hrd_params_present_flag : " << ( sps->sps_timing_hrd_params_present_flag ? "1" : "0");

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
                    DLOG(INFO) << "## sps_sublayer_cpb_params_present_flag : " << ( sps->sps_sublayer_cpb_params_present_flag ? "1" : "0");

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

              TRUE_OR_RETURN(br->ReadBool(&sps->sps_field_seq_flag));
              DLOG(INFO) << "## sps_field_seq_flag : " << ( sps->sps_field_seq_flag ? "1" : "0");

              TRUE_OR_RETURN(br->ReadBool(&sps->sps_vui_parameters_present_flag));
               DLOG(INFO) << "## sps_vui_parameters_present_flag : " << ( sps->sps_vui_parameters_present_flag ? "1" : "0");

              if(sps->sps_vui_parameters_present_flag){
                  TRUE_OR_RETURN(br->ReadUE(&sps->sps_vui_payload_size_minus1));
                  bool tmp_sps_vui_alignment_zero_bit=false;

                  while(! br->byte_aligned( )){
                    TRUE_OR_RETURN(br->ReadBool(&tmp_sps_vui_alignment_zero_bit));
                    //sps->sps_vui_alignment_zero_bit = tmp_sps_vui_alignment_zero_bit;
                  }
                  OK_OR_RETURN(Vui_Payload(sps->max_sublayers_minus1, br, &sps->vui_parameters));
              }
              TRUE_OR_RETURN(br->ReadBool(&sps->sps_extension_flag));
              DLOG(INFO) << "## sps_extension_flag : " << ( sps->sps_extension_flag ? "1" : "0");


              if(sps->sps_extension_flag){
                  TRUE_OR_RETURN(br->ReadBool(&sps->sps_range_extension_flag));
                  TRUE_OR_RETURN(br->ReadBits(7,&sps->sps_extension_7bits));
                  if( sps->sps_range_extension_flag ){
                      //todo
                      //sps_range_extension( )
                      if(!sps->sre){
                        sps->sre.emplace();
                      }
                      SpsRangeExtension(br,sps->sps_max_luma_transform_size_64_flag,&sps->sre.value());
                  }
                }
                if(sps->sps_extension_7bits){

                  bool tmp_sps_extended_precision_flag = false;
                  while(br->more_rbsp_data()){
                    TRUE_OR_RETURN(br->ReadBool(&tmp_sps_extended_precision_flag));
                  }
                }

                //OK_OR_RETURN(rbsp_trailing_bits(br));
                rbsp_trailing_bits(br);




  // This will replace any existing SPS instance.
  *sps_id = sps->sps_seq_parameter_set_id;
  DLOG(INFO) << "## Replace Old Sps Instance  by This Sps IP : " <<  sps->sps_seq_parameter_set_id;

  //active_spses_[*sps_id] = std::move(sps);
  active_spses_.emplace(*sps_id, std::move(sps));
  if (!HasSps(*sps_id)){
    DLOG(ERROR) << "# Back Up Sps Instance no Work";
  }

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

  //std::unique_ptr<H266Sps> sps(new H266Sps);
   //auto sps_list = GetSpsForVps(*vps_id);


  TRUE_OR_RETURN(br->ReadBits(4, &vps->vps_video_parameter_set_id));

      LOG(INFO) << " ## vps_video_parameter_set_id : " << vps->vps_video_parameter_set_id;




  //const
  H266Sps* sps = GetSps(*vps_id);

   if(!sps){
      sps =GetFirstSps();
   }
  if (!sps) {
    LOG(INFO) << "Parsing H.266 ERROR none sps use this vps need investigate ";
    return kOk;
  }




  TRUE_OR_RETURN(br->ReadBits(6, &vps->vps_max_layers_minus1));
  TRUE_OR_RETURN(br->ReadBits(3, &vps->vps_max_sublayers_minus1));
  vps->vps_max_sublayers = vps->vps_max_layers_minus1 + 1;
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
              for( uint32_t j = 0; j <= vps->vps_max_layers_minus1; j++ ){
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
      int tmp_vps_ols_ptl_idx = 0;
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
          if(!vps->vps_dpd.has_value()){
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
      if (!vps->vps_general_timing_hrd_parameters.has_value()) {
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
        if (!vps->vps_ols_parameters.has_value()) {
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
  //DisplayH266VPS(*vps);
  // This will replace any existing VPS instance.


  //force to sigle layer
  vps->vps_max_layers_minus1 = 0;
  *vps_id = vps->vps_video_parameter_set_id;
  //active_vpses_[*vps_id] = std::move(vps);
  DLOG(INFO) << " We remplace Old Vps instance by the new witih ID :" <<  vps->vps_video_parameter_set_id;

  active_vpses_.emplace(*vps_id, std::move(vps));
  if (!HasVps(*vps_id)){
    DLOG(ERROR) << "# Back Up Vps Instance no Work";
  }

  return kOk;
}
#endif

H266Parser::Result H266Parser::ParseScalingListData(H26xBitReader* br, H266Scalinglistdata* scaling_data, bool chroma_present) {
  // H.266 supporte plusieurs matrices de scaling list
  // Ici, un exemple simplifié
// H.266 has 28 scaling lists (0-27)
  int num_lists = 28;
  // 7.4.3.4, deriving DiagScanOrder
    static const uint8_t DiagScanOrder[64][2] = {
        { 0,  0, }, { 0,  1, }, { 1,  0, }, { 0,  2, }, { 1,  1, }, { 2,  0, }, { 0,  3, }, { 1,  2, },
        { 2,  1, }, { 3,  0, }, { 0,  4, }, { 1,  3, }, { 2,  2, }, { 3,  1, }, { 4,  0, }, { 0,  5, },
        { 1,  4, }, { 2,  3, }, { 3,  2, }, { 4,  1, }, { 5,  0, }, { 0,  6, }, { 1,  5, }, { 2,  4, },
        { 3,  3, }, { 4,  2, }, { 5,  1, }, { 6,  0, }, { 0,  7, }, { 1,  6, }, { 2,  5, }, { 3,  4, },
        { 4,  3, }, { 5,  2, }, { 6,  1, }, { 7,  0, }, { 1,  7, }, { 2,  6, }, { 3,  5, }, { 4,  4, },
        { 5,  3, }, { 6,  2, }, { 7,  1, }, { 2,  7, }, { 3,  6, }, { 4,  5, }, { 5,  4, }, { 6,  3, },
        { 7,  2, }, { 3,  7, }, { 4,  6, }, { 5,  5, }, { 6,  4, }, { 7,  3, }, { 4,  7, }, { 5,  6, },
        { 6,  5, }, { 7,  4, }, { 5,  7, }, { 6,  6, }, { 7,  5, }, { 6,  7, }, { 7,  6, }, { 7,  7, }, };

  for (int id = 0; id < num_lists; id++) {
    int matrixSize = id < 2 ? 2 : ( id < 8 ? 4 : 8 );
    //aps_chroma_present_flag
    if( chroma_present || id % 3 == 2 || id == 27 ) {

      bool copy_mode_flag;
      TRUE_OR_RETURN(br->ReadBool(&copy_mode_flag));
      scaling_data->scaling_list_copy_mode_flag.push_back(copy_mode_flag);

      if (!copy_mode_flag) {
        bool pred_mode_flag;
        TRUE_OR_RETURN(br->ReadBool(&pred_mode_flag));
        scaling_data->scaling_list_pred_mode_flag.push_back(pred_mode_flag);
      }
      if( ( scaling_data->scaling_list_copy_mode_flag[id] || scaling_data->scaling_list_pred_mode_flag[id] ) && id != 0 && id != 2 && id != 8 ){
        int pred_id_delta;
        TRUE_OR_RETURN(br->ReadUE(&pred_id_delta));
        scaling_data->scaling_list_pred_id_delta.push_back(pred_id_delta);
        if (pred_id_delta == 0){
          int nextCoef = 0;
          if( id > 13 ) {
            size_t idx = id - 14;
            if (idx >= scaling_data->scaling_list_dc_coef.size()) {
              scaling_data->scaling_list_dc_coef.resize(idx + 1);
            }

            int tmp_scaling_list_dc_coef = 0;
            TRUE_OR_RETURN(br->ReadSE(&tmp_scaling_list_dc_coef));
            // check im not sure
            scaling_data->scaling_list_dc_coef[idx] = tmp_scaling_list_dc_coef;
          }
          for( int i = 0; i < matrixSize * matrixSize; i++ ) {
            int x = DiagScanOrder[i][0];
            int y = DiagScanOrder[i][1];
            if( !( id > 25 && x >= 4 && y >= 4 ) ) {
              int tmp_scaling_list_delta_coef = 0;
              std::vector<int> tmp_vector;

              if(static_cast<size_t>(id) > scaling_data->scaling_list_delta_coef.size()){
                scaling_data->scaling_list_delta_coef.resize(id + 1);
              }

              TRUE_OR_RETURN(br->ReadSE(&tmp_scaling_list_delta_coef));
              tmp_vector.push_back(tmp_scaling_list_delta_coef);

              //scaling_data->scaling_list_delta_coef.push_back(tmp_scaling_list_delta_coef);
              scaling_data->scaling_list_delta_coef.push_back(tmp_vector);



              nextCoef += tmp_scaling_list_delta_coef;
              //scaling_data->scaling_list_delta_coef[ id ][ i ];
            }
            if (scaling_data->ScalingList.size() <= static_cast<size_t>(id)) {
                scaling_data->ScalingList.resize(id + 1);
                scaling_data->ScalingList[id].resize(matrixSize * matrixSize);
            }
            scaling_data->ScalingList[id][i] = nextCoef;
          }
        }
      } else {
        scaling_data->scaling_list_pred_id_delta.push_back(0);
        if (scaling_data->scaling_list_delta_coef.size() <= static_cast<size_t>(id)) {
          scaling_data->scaling_list_delta_coef.resize(id + 1);
        }
      }
    } else {
      scaling_data->scaling_list_copy_mode_flag.push_back(false);
      scaling_data->scaling_list_pred_mode_flag.push_back(false);
      scaling_data->scaling_list_pred_id_delta.push_back(0);
    }
  }
  return kOk;
}




H266Parser::Result H266Parser::ParseLmcsData(H26xBitReader* br, H266LmcsData* lmcs_data, bool chroma_present) {
  TRUE_OR_RETURN(br->ReadUE(&lmcs_data->lmcs_min_bin_idx));
  DLOG(INFO) << "## lmcs_min_bin_idx: " << lmcs_data->lmcs_min_bin_idx;

  TRUE_OR_RETURN(br->ReadUE(&lmcs_data->lmcs_delta_max_bin_idx));
  DLOG(INFO) << "## lmcs_delta_max_bin_idx: " << lmcs_data->lmcs_delta_max_bin_idx;

  TRUE_OR_RETURN(br->ReadUE(&lmcs_data->lmcs_delta_cw_prec_minus1));
  DLOG(INFO) << "## lmcs_delta_cw_prec_minus1: " << lmcs_data->lmcs_delta_cw_prec_minus1;

  int max_bin_idx = 15;  // from spec
  int min_bin_idx = lmcs_data->lmcs_min_bin_idx;
  int max_bin = max_bin_idx - lmcs_data->lmcs_delta_max_bin_idx;

  // Parser les delta CW values
  for (int i = min_bin_idx; i <= max_bin; i++) {
    int tmp_delta_abs_cw;
    int len_lmcs_delta_cw_prec_minus1  = lmcs_data->lmcs_delta_cw_prec_minus1 + 1;
    TRUE_OR_RETURN(br->ReadBits(len_lmcs_delta_cw_prec_minus1, &tmp_delta_abs_cw));
    lmcs_data->lmcs_delta_abs_cw.push_back(tmp_delta_abs_cw);
    DLOG(INFO) << "## lmcs_delta_abs_cw: " << tmp_delta_abs_cw;


    bool delta_sign_flag = false;
    if (tmp_delta_abs_cw > 0) {
      TRUE_OR_RETURN(br->ReadBool(&delta_sign_flag));
    }
    lmcs_data->lmcs_delta_sign_cw_flag.push_back(delta_sign_flag);
  }
  //aps_chroma_present_flag
  if (chroma_present) {
    TRUE_OR_RETURN(br->ReadBits(3, &lmcs_data->lmcs_delta_abs_crs));
    DLOG(INFO) << "## lmcs_delta_abs_crs: " << lmcs_data->lmcs_delta_abs_crs;

    if (lmcs_data->lmcs_delta_abs_crs > 0) {
      TRUE_OR_RETURN(br->ReadBool(&lmcs_data->lmcs_delta_sign_crs_flag));
      DLOG(INFO) << "## lmcs_delta_sign_crs_flag: " << lmcs_data->lmcs_delta_sign_crs_flag;
    } else {
      lmcs_data->lmcs_delta_sign_crs_flag = false;
    }
  } else {
    lmcs_data->lmcs_delta_abs_crs = 0;
    lmcs_data->lmcs_delta_sign_crs_flag = false;
  }
  lmcs_data->lmcs_max_bin_idx = max_bin;

  return kOk;
}


H266Parser::Result H266Parser::ParseAlfData(H26xBitReader* br, H266AlfData* alf_data, bool chroma_present) {
  // Parse flags de base
  TRUE_OR_RETURN(br->ReadBool(&alf_data->alf_luma_filter_signal_flag));
  DLOG(INFO) << "## alf_luma_filter_signal_flag : " << (alf_data->alf_luma_filter_signal_flag  ? "1" : "0");


  if (chroma_present) {
    TRUE_OR_RETURN(br->ReadBool(&alf_data->alf_chroma_filter_signal_flag));
    DLOG(INFO) << "## alf_chroma_filter_signal_flag: " << alf_data->alf_chroma_filter_signal_flag;

    if (alf_data->alf_chroma_filter_signal_flag) {
      TRUE_OR_RETURN(br->ReadBool(&alf_data->alf_cc_cb_filter_signal_flag));
      DLOG(INFO) << "## alf_cc_cb_filter_signal_flag: " << alf_data->alf_cc_cb_filter_signal_flag;

      TRUE_OR_RETURN(br->ReadBool(&alf_data->alf_cc_cr_filter_signal_flag));
      DLOG(INFO) << "## alf_cc_cr_filter_signal_flag: " << alf_data->alf_cc_cr_filter_signal_flag;
    }
  }

  if (alf_data->alf_luma_filter_signal_flag) {
    // Parse données Luma
    TRUE_OR_RETURN(br->ReadBool(&alf_data->alf_luma_clip_flag));
    DLOG(INFO) << "## alf_luma_clip_flag: " << alf_data->alf_luma_clip_flag;

    TRUE_OR_RETURN(br->ReadUE(&alf_data->alf_luma_num_filters_signalled_minus1));
    DLOG(INFO) << "## alf_luma_num_filters_signalled_minus1: " << alf_data->alf_luma_num_filters_signalled_minus1;

    int num_luma_filters = alf_data->alf_luma_num_filters_signalled_minus1 + 1;
    if( alf_data->alf_luma_num_filters_signalled_minus1 > 0 ) {
        // Parser les indices delta pour chaque filtre
        for (int i = 0; i < num_luma_filters; i++) {
          int delta_idx;
          int len_alf_luma_coeff_delta_idx = 0;
          while ((1 << len_alf_luma_coeff_delta_idx) < num_luma_filters) {
            len_alf_luma_coeff_delta_idx++;
          }
          TRUE_OR_RETURN(br->ReadBits(len_alf_luma_coeff_delta_idx,&delta_idx));
          alf_data->alf_luma_coeff_delta_idx.push_back(delta_idx);
        }
        DLOG(INFO) << "## Parsed " << num_luma_filters << " luma delta indices";
    }
    for( int sfIdx = 0; sfIdx <= alf_data->alf_luma_num_filters_signalled_minus1; sfIdx++ ){
         /************ set conf after reading vvc std   ************************ */
        // Nombre total de coefficients (dépend de la spécification)
        int total_coeffs = 12;  //from  spec/
        int total_filters = 25; // from spec

        // Parser les coefficients absolus
        for (int i = 0; i < total_coeffs * total_filters; i++) {
          int coeff_abs;
          TRUE_OR_RETURN(br->ReadBits(8, &coeff_abs));  // 8 bits par coefficient
          //alf_luma_coeff_abs[ sfIdx ][ j ]
          alf_data->alf_luma_coeff_abs.push_back(coeff_abs);
          if(coeff_abs){
             int coeff_sign;
            TRUE_OR_RETURN(br->ReadBits(1, &coeff_sign));  // 1 bit par signe
            //alf_luma_coeff_sign[ sfIdx ][ j ]
            alf_data->alf_luma_coeff_sign.push_back(coeff_sign);
            //DLOG(INFO) << "## alf_data->alf_luma_coeff_sign : " << alf_data->alf_luma_coeff_sign;
          }
        }
    }


    if (alf_data->alf_luma_clip_flag) {
      // Parser les indices de clip
     for (int i = 0; i < alf_data->alf_luma_num_filters_signalled_minus1; i++) {
        for( int j = 0; j < 12; j++ ){
            int clip_idx;
            TRUE_OR_RETURN(br->ReadBits(2, &clip_idx));  // 2 bits par index de clip
            alf_data->alf_luma_clip_idx.push_back(clip_idx);
           // DLOG(INFO) << "## alf_data->alf_luma_clip_idx : " << alf_data->alf_luma_clip_idx;

        }
      }
    }
  }


  if (alf_data->alf_chroma_filter_signal_flag && chroma_present) {
    // Parse données Chroma
    TRUE_OR_RETURN(br->ReadBool(&alf_data->alf_chroma_clip_flag));
    DLOG(INFO) << "## alf_chroma_clip_flag : " << ( alf_data->alf_chroma_clip_flag ? "1" : "0");


    TRUE_OR_RETURN(br->ReadUE(&alf_data->alf_chroma_num_alt_filters_minus1));
    DLOG(INFO) << "## alf_chroma_num_alt_filters_minus1: " << alf_data->alf_chroma_num_alt_filters_minus1;

    int num_chroma_filters = alf_data->alf_chroma_num_alt_filters_minus1 + 1;
    int chroma_coeffs = 6;  // from spec

    for (int f = 0; f < num_chroma_filters; f++) {
      std::vector<int> coeff_abs_row;
      std::vector<int> coeff_sign_row;
      std::vector<int> clip_idx_row;

      for (int c = 0; c < chroma_coeffs; c++) {
        int coeff_abs;
        TRUE_OR_RETURN(br->ReadUE(&coeff_abs));
        coeff_abs_row.push_back(coeff_abs);

        int coeff_sign;
        TRUE_OR_RETURN(br->ReadBits(1, &coeff_sign));
        coeff_sign_row.push_back(coeff_sign);

        if (alf_data->alf_chroma_clip_flag) {
          for( int j = 0; j < 6; j++){
            int clip_idx;
            TRUE_OR_RETURN(br->ReadBits(2, &clip_idx));
            clip_idx_row.push_back(clip_idx);
          }
        }
      }

      alf_data->alf_chroma_coeff_abs.push_back(coeff_abs_row);
      alf_data->alf_chroma_coeff_sign.push_back(coeff_sign_row);
      if (alf_data->alf_chroma_clip_flag) {
        alf_data->alf_chroma_clip_idx.push_back(clip_idx_row);
      } else {
        //alf_data->alf_chroma_clip_idx.push_back(0);
        std::vector<int> clip_vec;
        clip_vec.push_back(0);
        alf_data->alf_chroma_clip_idx.push_back(clip_vec);

      }
    }


    // Parse Cross-Component filters si présents
    if (alf_data->alf_cc_cb_filter_signal_flag) {
      TRUE_OR_RETURN(br->ReadUE(&alf_data->alf_cc_cb_filters_signalled_minus1));
      DLOG(INFO) << "## alf_data->alf_cc_cb_filters_signalled_minus1: " << alf_data->alf_cc_cb_filters_signalled_minus1;

      int num_cc_cb_filters = alf_data->alf_cc_cb_filters_signalled_minus1 + 1;

      for (int f = 0; f < num_cc_cb_filters; f++) {
        std::vector<int> mapped_coeffs;
        std::vector<int> coeff_signs;

        for (int c = 0; c < 7; c++) {  // from spec
          int coeff_abs;
          TRUE_OR_RETURN(br->ReadBits(3, &coeff_abs));
          mapped_coeffs.push_back(coeff_abs);
          if(coeff_abs){
            int coeff_sign;
            TRUE_OR_RETURN(br->ReadBits(1, &coeff_sign));
            coeff_signs.push_back(coeff_sign);
          } else {
            coeff_signs.push_back(0);
          }
        }

        alf_data->alf_cc_cb_mapped_coeff_abs.push_back(mapped_coeffs);
        alf_data->alf_cc_cb_coeff_sign.push_back(coeff_signs);
      }
    }

    if (alf_data->alf_cc_cr_filter_signal_flag) {
      TRUE_OR_RETURN(br->ReadUE(&alf_data->alf_cc_cr_filters_signalled_minus1));
      int num_cc_cr_filters = alf_data->alf_cc_cr_filters_signalled_minus1 + 1;
      DLOG(INFO) << "## alf_data->alf_cc_cr_filters_signalled_minus1 : " << alf_data->alf_cc_cr_filters_signalled_minus1;


      for (int f = 0; f < num_cc_cr_filters; f++) {
        std::vector<int> mapped_coeffs;
        std::vector<int> coeff_signs;

        for (int c = 0; c < 7; c++) {  // from spec
          int coeff_abs;
          TRUE_OR_RETURN(br->ReadBits(3, &coeff_abs));
          mapped_coeffs.push_back(coeff_abs);
          if (coeff_abs){
            int coeff_sign;
            TRUE_OR_RETURN(br->ReadBits(1, &coeff_sign));
            coeff_signs.push_back(coeff_sign);
          } else {
              coeff_signs.push_back(0);
          }
        }

        alf_data->alf_cc_cr_mapped_coeff_abs.push_back(mapped_coeffs);
        alf_data->alf_cc_cr_coeff_sign.push_back(coeff_signs);
      }
    }
  }

  return kOk;
}




H266Parser::Result H266Parser::ParseAps(const Nalu& nalu, int* aps_id, int* aps_type) {
  DCHECK(nalu.type() == Nalu::H266_PREFIX_APS_NUT ||
         nalu.type() == Nalu::H266_SUFFIX_APS_NUT);
  //7.3.2.6 Adaptation parameter set RBSP syntax
  LOG(INFO) << "Parsing H.266 APS Adaptation parameter set NALU";

  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  *aps_id = -1;
  *aps_type = -1;
  std::unique_ptr<H266Aps> aps(new H266Aps);
  TRUE_OR_RETURN(aps);

  int temp = 0;
  TRUE_OR_RETURN(br->ReadUE(&temp));
  aps->aps_params_type = static_cast<uint8_t>(temp);

  DLOG(INFO) << "## aps_params_type : " << aps->aps_params_type;

   int tmp_aps_adaptation_parameter_set_id = 0;
  TRUE_OR_RETURN(br->ReadUE(&tmp_aps_adaptation_parameter_set_id));
  aps->aps_adaptation_parameter_set_id = static_cast<uint8_t>(tmp_aps_adaptation_parameter_set_id);


  DLOG(INFO) << "## aps_adaptation_parameter_set_id : " << aps->aps_adaptation_parameter_set_id;

  TRUE_OR_RETURN(br->ReadBool(&aps->aps_chroma_present_flag));
  DLOG(INFO) << "## aps_chroma_present_flag : " << ( aps->aps_chroma_present_flag ? "1" : "0");
  if(aps->aps_params_type == KvvcALFAPS){
    DLOG(INFO) << " Processing ALF APS";
    aps->alfd = H266AlfData();
    TRUE_OR_RETURN(ParseAlfData(br, &aps->alfd.value(), aps->aps_chroma_present_flag));

  } else if(aps->aps_params_type == KvvcLMCSAPS){
    DLOG(INFO) << " Processing LMCS APS";
    aps->lmcsd = H266LmcsData();
    TRUE_OR_RETURN(ParseLmcsData(br, &aps->lmcsd.value(), aps->aps_chroma_present_flag));


  } else if(aps->aps_params_type == KvvcSCALINGAPS){
    DLOG(INFO) << " Processing SCALING APS";
    aps->sld = H266Scalinglistdata();
    TRUE_OR_RETURN(ParseScalingListData(br, &aps->sld.value(), aps->aps_chroma_present_flag));

  } else {
    DLOG(INFO) << " INVALID APS TYPE";
  }
  TRUE_OR_RETURN(br->ReadBool(&aps->aps_extension_flag));
  DLOG(INFO) << "## aps_extension_flag : " << ( aps->aps_extension_flag ? "1" : "0");
  while(br->more_rbsp_data()){
      TRUE_OR_RETURN(br->ReadBool(&aps->aps_extension_data_flag));
  }
  OK_OR_RETURN(rbsp_trailing_bits(br));

  *aps_id = aps->aps_adaptation_parameter_set_id;
  *aps_type = aps->aps_params_type;
  //active_apses_[*aps_id] = std::move(aps);

  active_apses_.emplace(*aps_id, std::move(aps));

  return kOk;
}



H266Parser::Result H266Parser::ParsePictureHeaderStructure(const Nalu& nalu,
                                                  H266PictureHeaderStructure* phs) {
  //DCHECK_EQ(Nalu::H266_PH_NUT, nalu.type());
    LOG(INFO) << "Parsing H.266 Picture Header NALU : " << nalu.type();
    LOG(INFO) << " Parsing Parsing PictureHeader Structure ";
 // disable to check it


  LOG(INFO) << "Parsing H.266 Picture Header NALU";
  H26xBitReader reader;
  reader.Initialize(nalu.data() + nalu.header_size(), nalu.payload_size());
  H26xBitReader* br = &reader;

  bool tmp_ph_gdr_or_irap_pic_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_ph_gdr_or_irap_pic_flag));
  phs->ph_gdr_or_irap_pic_flag = tmp_ph_gdr_or_irap_pic_flag;
  DLOG(INFO) << "## ph_gdr_or_irap_pic_flag : " << (phs->ph_gdr_or_irap_pic_flag ? "1" : "0");

  bool tmp_ph_non_ref_pic_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_ph_non_ref_pic_flag));
  phs->ph_non_ref_pic_flag = tmp_ph_non_ref_pic_flag;
  DLOG(INFO) << "## ph_non_ref_pic_flag : " << (phs->ph_non_ref_pic_flag ? "1" : "0");

  if(phs->ph_gdr_or_irap_pic_flag){
    bool tmp_ph_gdr_pic_flag = false;
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_gdr_pic_flag));
    DLOG(INFO) << "## ph_gdr_pic_flag : " << ( phs->ph_gdr_pic_flag ? "1" : "0");

    phs->ph_gdr_pic_flag = tmp_ph_gdr_pic_flag;
  } else {
    phs->ph_gdr_pic_flag = false;
  }

  bool tmp_ph_inter_slice_allowed_flag = false;
  TRUE_OR_RETURN(br->ReadBool(&tmp_ph_inter_slice_allowed_flag));
  phs->ph_inter_slice_allowed_flag = tmp_ph_inter_slice_allowed_flag;
  DLOG(INFO) << "## ph_inter_slice_allowed_flag : " << (phs->ph_inter_slice_allowed_flag ? "1" : "0");


  if(phs->ph_inter_slice_allowed_flag){
    bool tmp_ph_intra_slice_allowed_flag = false;
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_intra_slice_allowed_flag));
    phs->ph_intra_slice_allowed_flag = tmp_ph_intra_slice_allowed_flag;
    DLOG(INFO) << "## ph_intra_slice_allowed_flag: " << (phs->ph_intra_slice_allowed_flag ? "1" : "0");
  } else {
    phs->ph_intra_slice_allowed_flag = true;
  }

  int tmp_ph_pic_parameter_set_id = 0;
  TRUE_OR_RETURN(br->ReadUE(&tmp_ph_pic_parameter_set_id));
  phs->ph_pic_parameter_set_id = tmp_ph_pic_parameter_set_id;
  DLOG(INFO) << "## phs->ph_pic_parameter_set_id : " << phs->ph_pic_parameter_set_id;
  DLOG(INFO) << " >> GetPps from ph_pic_parameter_set_id " << phs->ph_pic_parameter_set_id;

  for (const auto& pair : active_ppses_) {
  DLOG(INFO) << "  - PPS id " << pair.first
             << " valid=" << (pair.second != nullptr);
  }



 //if (!HasPps(phs->ph_pic_parameter_set_id - 1)) {
 if (!HasPps(phs->ph_pic_parameter_set_id -1 )) {

     DLOG(ERROR) << "PPS " << phs->ph_pic_parameter_set_id << " not found";
     DebugPrintAvailableSets();
     return kInvalidStream;
 }
H266Pps* pps = GetPps(phs->ph_pic_parameter_set_id - 1);
//H266Pps* pps = GetPps(phs->ph_pic_parameter_set_id);

   if (!pps) {
     DLOG(ERROR) << "GetPps returned nullptr";
     return kInvalidStream;
   }
   if (!pps) {
     DLOG(WARNING) << "Trying first available PPS";
     pps = GetFirstPps();
     if (!pps) {
       DLOG(ERROR) << "No PPS available at all";
       return kInvalidStream;
     }
   }
  TRUE_OR_RETURN(pps);


  LOG(INFO) << "Parsing H.266 Picture Header NALU" << "pps" << pps << "ph_pic_parameter_set_id " << phs->ph_pic_parameter_set_id;

   //const
    DLOG(INFO) << " >> GetSps from pps_seq_parameter_set_id " << pps->pps_seq_parameter_set_id;

   H266Sps* sps = GetSps(pps->pps_seq_parameter_set_id);
  LOG(INFO) << "Parsing H.266 Picture Header NALU" << "sps  " << sps << "pps_seq_parameter_set_id " << pps->pps_seq_parameter_set_id;
  if (!sps){
    sps = GetFirstSps();
    LOG(INFO) << "We try another Parse Sps in using GetFirstSps func";
  }

  if (!sps) {
    LOG(ERROR) << "SPS " << pps->pps_seq_parameter_set_id
               << " referenced by PPS " << phs->ph_pic_parameter_set_id << " not found";
    return kInvalidStream;
  }
  TRUE_OR_RETURN(sps);

  //  add not sure
  if (!HasVps(sps->sps_video_parameter_set_id )) {

     DLOG(ERROR) << "PPS " << phs->ph_pic_parameter_set_id << " not found";
     DebugPrintAvailableSets();
     return kInvalidStream;
  }

  H266Vps* vps = GetVps(sps->sps_video_parameter_set_id);
  if (!vps){
    vps = GetFirstVps();
    LOG(INFO) << "We try another Parse Vps in using GetFirstVps func func";
  }



  LOG(INFO) << "Parsing H.266 Picture Header NALU" << "sps : " << sps << "ph_pic_parameter_set_id :" << pps->seq_parameter_set_id;

  int len_ph_pic_order_cnt_lsb = sps->sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
  TRUE_OR_RETURN(br->ReadBits(len_ph_pic_order_cnt_lsb,&phs->ph_pic_order_cnt_lsb));
  DLOG(INFO) << "## ph_pic_order_cnt_lsb : " << phs->ph_pic_order_cnt_lsb;

  if(phs->ph_gdr_pic_flag){
    TRUE_OR_RETURN(br->ReadUE(&phs->ph_recovery_poc_cnt));
    DLOG(INFO) << "## ph_recovery_poc_cnt : " << phs->ph_recovery_poc_cnt;

  }
  /************************************************/
  //Page 107
  int NumExtraPhBits = 0;
  int max_extra_bytes = sps->sps_num_extra_ph_bytes * 8;

  /* if (max_extra_bytes > 16){
    LOG(ERROR) << "Invalid sps_num_extra_ph_bytes: " << max_extra_bytes;
    return kInvalidStream;
  } */

  DLOG(INFO) << " ## max_extra_bytes :" << max_extra_bytes;


  ///  add to try fix it  to realign bitstream
  for( int i = 0; i < max_extra_bytes; i++ ){
    bool tmp_sps_extra_ph_bit_present_flag = false;
    TRUE_OR_RETURN(br->ReadBool(&tmp_sps_extra_ph_bit_present_flag));
    DLOG(INFO) << "## sps_extra_ph_bit_present_flag : " << (tmp_sps_extra_ph_bit_present_flag ? "1" : "0");

    sps->sps_extra_ph_bit_present_flag.push_back(tmp_sps_extra_ph_bit_present_flag);

    if( sps->sps_extra_ph_bit_present_flag[i]){
      NumExtraPhBits++;
    }
  }
  /************************************************/
  phs->ph_extra_bit.resize(NumExtraPhBits);
  for( int i = 0; i < NumExtraPhBits; i++ ){
    bool tmp_ph_extra_bit;
    TRUE_OR_RETURN(br->ReadBool(&tmp_ph_extra_bit));
    DLOG(INFO) << "## ph_extra_bit : " << tmp_ph_extra_bit;
    phs->ph_extra_bit.push_back(tmp_ph_extra_bit);
  }
  if( sps->sps_poc_msb_cycle_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_poc_msb_cycle_present_flag));
    DLOG(INFO) << "## ph_poc_msb_cycle_present_flag: " << ( phs->ph_poc_msb_cycle_present_flag ? "1" : "0");

    if(phs->ph_poc_msb_cycle_present_flag){
      int len_ph_poc_msb_cycle_val = sps->sps_poc_msb_cycle_len_minus1 + 1;
      TRUE_OR_RETURN(br->ReadBits(len_ph_poc_msb_cycle_val,&phs->ph_poc_msb_cycle_val));
      DLOG(INFO) << "## ph_poc_msb_cycle_val  : " << phs->ph_poc_msb_cycle_val;

    }
  }
  if( sps->sps_alf_enabled_flag && pps->pps_alf_info_in_ph_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_enabled_flag));
    if(phs->ph_alf_enabled_flag){
      TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_num_alf_aps_ids_luma));
      DLOG(INFO) << "## ph_num_alf_aps_ids_luma : " << phs->ph_num_alf_aps_ids_luma;

      int max_ph_num_alf_aps_ids_luma = phs->ph_num_alf_aps_ids_luma;
      for( int i = 0; i < max_ph_num_alf_aps_ids_luma; i++ ){
        int tmp_ph_alf_aps_id_luma = 0;
        TRUE_OR_RETURN(br->ReadBits(3,&tmp_ph_alf_aps_id_luma));
        phs->ph_alf_aps_id_luma.push_back(tmp_ph_alf_aps_id_luma);
        DLOG(INFO) << "## ph_alf_aps_id_luma : " << tmp_ph_alf_aps_id_luma;
      }
      if( sps->sps_chroma_format_idc != 0 ) {
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cb_enabled_flag));
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cr_enabled_flag));
      } else {
        phs->ph_alf_cb_enabled_flag = false;
        phs->ph_alf_cr_enabled_flag = false;
      }
      DLOG(INFO) << "## ph_alf_cb_enabled_flag : " << ( phs->ph_alf_cb_enabled_flag ? "1" : "0");
      DLOG(INFO) << "## ph_alf_cr_enabled_flag : " << ( phs->ph_alf_cr_enabled_flag ? "1" : "0");


      if( phs->ph_alf_cb_enabled_flag || phs->ph_alf_cr_enabled_flag ){
          uint32_t tmp_alf_aps_id_chroma;
          TRUE_OR_RETURN(br->ReadBits(3, &tmp_alf_aps_id_chroma));
          phs->ph_alf_aps_id_chroma = tmp_alf_aps_id_chroma;
          DLOG(INFO) << "## ph_alf_aps_id_chroma : " << phs->ph_alf_aps_id_chroma;

      }
      if (sps->sps_ccalf_enabled_flag ) {
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cc_cb_enabled_flag));
        if(phs->ph_alf_cc_cb_enabled_flag){
          TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_alf_cc_cb_aps_id));
          DLOG(INFO) << "## ph_alf_cc_cb_aps_id : " << phs->ph_alf_cc_cb_aps_id;

        }
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_alf_cc_cr_enabled_flag));
        if(phs->ph_alf_cc_cr_enabled_flag){
          TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_alf_cc_cr_aps_id));
          DLOG(INFO) << "## ph_alf_cc_cr_aps_id : " << phs->ph_alf_cc_cr_aps_id;

        }
      }
    }
  } else {
    phs->ph_alf_enabled_flag = false;
  }

  if( sps->sps_lmcs_enabled_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_lmcs_enabled_flag));
    if(phs->ph_lmcs_enabled_flag){
      TRUE_OR_RETURN(br->ReadBits(2,&phs->ph_lmcs_aps_id));
      DLOG(INFO) << "## ph_lmcs_aps_id : " << phs->ph_lmcs_aps_id;

      if ( sps->sps_chroma_format_idc != 0 ){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_chroma_residual_scale_flag));
      } else {
        phs->ph_chroma_residual_scale_flag = false;
      }
    }
  } else {
    phs->ph_lmcs_enabled_flag = false;
    phs->ph_chroma_residual_scale_flag = false;
  }


  if( sps->sps_explicit_scaling_list_enabled_flag ) {
       bool tmp_ph_explicit_scaling_list_enabled_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_ph_explicit_scaling_list_enabled_flag));
      phs->ph_explicit_scaling_list_enabled_flag = tmp_ph_explicit_scaling_list_enabled_flag;
      DLOG(INFO) << "## ph_explicit_scaling_list_enabled_flag : " << ( tmp_ph_explicit_scaling_list_enabled_flag ? "1" : "0");
      if(phs->ph_explicit_scaling_list_enabled_flag){
        TRUE_OR_RETURN(br->ReadBits(3,&phs->ph_scaling_list_aps_id));
        DLOG(INFO) << "## ph_scaling_list_aps_id  : " << phs->ph_scaling_list_aps_id;
      }
  } else {
    phs->ph_explicit_scaling_list_enabled_flag = false;
  }

  if( sps->sps_virtual_boundaries_enabled_flag && !sps->sps_virtual_boundaries_present_flag ) {
    TRUE_OR_RETURN(br->ReadBool(&phs->ph_virtual_boundaries_present_flag));
    DLOG(INFO) << "## ph_virtual_boundaries_present_flag : " << ( phs->ph_virtual_boundaries_present_flag ? "1" : "0");

    if(phs->ph_virtual_boundaries_present_flag){
      TRUE_OR_RETURN(br->ReadUE(&phs->ph_num_ver_virtual_boundaries));
      DLOG(INFO) << "## ph_num_ver_virtual_boundaries : " << phs->ph_num_ver_virtual_boundaries;

      int max_ph_num_ver_virtual_boundaries = phs->ph_num_ver_virtual_boundaries;
      int tmp_ph_virtual_boundary_pos_x_minus1 = 0;
      for( int i = 0; i < max_ph_num_ver_virtual_boundaries; i++ ){
        TRUE_OR_RETURN(br->ReadUE(&tmp_ph_virtual_boundary_pos_x_minus1));
        DLOG(INFO) << "## ph_virtual_boundary_pos_x_minus1 : " << tmp_ph_virtual_boundary_pos_x_minus1;

        phs->ph_virtual_boundary_pos_x_minus1.push_back(tmp_ph_virtual_boundary_pos_x_minus1);
      }
      TRUE_OR_RETURN(br->ReadUE(&phs->ph_num_hor_virtual_boundaries));
      DLOG(INFO) << "## ph_num_hor_virtual_boundaries : " << phs->ph_num_hor_virtual_boundaries;


      int max_ph_num_hor_virtual_boundaries = phs->ph_num_hor_virtual_boundaries;
      int tmp_ph_virtual_boundary_pos_y_minus1 = 0;
      for( int i = 0; i < max_ph_num_hor_virtual_boundaries; i++ ){
        TRUE_OR_RETURN(br->ReadUE(&tmp_ph_virtual_boundary_pos_y_minus1));
        DLOG(INFO) << "## ph_virtual_boundary_pos_y_minus1 : " << tmp_ph_virtual_boundary_pos_y_minus1;

        phs->ph_virtual_boundary_pos_y_minus1.push_back(tmp_ph_virtual_boundary_pos_y_minus1);
      }
    } else {
      phs->ph_num_ver_virtual_boundaries = 0;
      phs->ph_num_hor_virtual_boundaries = 0;
    }

  }
    if( pps->pps_output_flag_present_flag && !phs->ph_non_ref_pic_flag ){
      TRUE_OR_RETURN(br->ReadBool(&phs->ph_pic_output_flag));
      DLOG(INFO) << "## ph_pic_output_flag : " << ( phs->ph_pic_output_flag ? "1" : "0");
    } else {
      phs->ph_pic_output_flag = true;
    }

    if( !pps->pps_rpl_info_in_ph_flag ){
      phs->rpl.emplace();

      //Ref_Pic_List(sps,pps,br,&phs->rpl.value());
      if (phs->rpl.has_value()) {
         Ref_Pic_List(*sps, *pps, br, &phs->rpl.value());
      }

    }
    if( sps->sps_partition_constraints_override_enabled_flag ){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_partition_constraints_override_flag));
        DLOG(INFO) << "## ph_partition_constraints_override_flag : " << ( phs->ph_partition_constraints_override_flag  ? "1" : "0");
    } else {
        phs->ph_partition_constraints_override_flag = false;
    }

    if( phs->ph_intra_slice_allowed_flag ) {
        if( phs->ph_partition_constraints_override_flag ) {
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_min_qt_min_cb_intra_slice_luma));
          DLOG(INFO) << "## ph_log2_diff_min_qt_min_cb_intra_slice_luma : " << phs->ph_log2_diff_min_qt_min_cb_intra_slice_luma;

          TRUE_OR_RETURN(br->ReadUE(&phs->ph_max_mtt_hierarchy_depth_intra_slice_luma));
          DLOG(INFO) << "## ph_max_mtt_hierarchy_depth_intra_slice_luma : " << phs->ph_max_mtt_hierarchy_depth_intra_slice_luma;


          if(phs->ph_max_mtt_hierarchy_depth_intra_slice_luma != 0){
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_bt_min_qt_intra_slice_luma));
            DLOG(INFO) << "## ph_log2_diff_max_bt_min_qt_intra_slice_luma : " << phs->ph_log2_diff_max_bt_min_qt_intra_slice_luma;

            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_tt_min_qt_intra_slice_luma));
            DLOG(INFO) << "## ph_log2_diff_max_tt_min_qt_intra_slice_luma : " << phs->ph_log2_diff_max_tt_min_qt_intra_slice_luma;

          } else {
            phs->ph_log2_diff_max_bt_min_qt_intra_slice_luma = 0;
            phs->ph_log2_diff_max_tt_min_qt_intra_slice_luma = 0;
          }

          if(sps->sps_qtbtt_dual_tree_intra_flag){
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_min_qt_min_cb_intra_slice_chroma));
            DLOG(INFO) << "## ph_log2_diff_min_qt_min_cb_intra_slice_chroma  : " << phs->ph_log2_diff_min_qt_min_cb_intra_slice_chroma;

            TRUE_OR_RETURN(br->ReadUE(&phs->ph_max_mtt_hierarchy_depth_intra_slice_chroma));
            DLOG(INFO) << "## ph_max_mtt_hierarchy_depth_intra_slice_chroma : " << phs->ph_max_mtt_hierarchy_depth_intra_slice_chroma;

            if(phs->ph_max_mtt_hierarchy_depth_intra_slice_chroma != 0 ){
              TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_bt_min_qt_intra_slice_chroma));
              DLOG(INFO) << "## ph_log2_diff_max_bt_min_qt_intra_slice_chroma : " << phs->ph_log2_diff_max_bt_min_qt_intra_slice_chroma;

              TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_tt_min_qt_intra_slice_chroma));
              DLOG(INFO) << "## ph_log2_diff_max_tt_min_qt_intra_slice_chroma  : " << phs->ph_log2_diff_max_tt_min_qt_intra_slice_chroma;

            } else {
              phs->ph_log2_diff_max_bt_min_qt_intra_slice_chroma = sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma;
              phs->ph_log2_diff_max_tt_min_qt_intra_slice_chroma = sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma;
            }

          }
        } else {
          phs->ph_log2_diff_min_qt_min_cb_intra_slice_luma = sps->sps_log2_diff_min_qt_min_cb_intra_slice_luma;
          phs->ph_max_mtt_hierarchy_depth_intra_slice_luma = sps->sps_max_mtt_hierarchy_depth_intra_slice_luma;
          phs->ph_log2_diff_max_bt_min_qt_intra_slice_luma = sps->sps_log2_diff_max_bt_min_qt_intra_slice_luma;
          phs->ph_log2_diff_max_tt_min_qt_intra_slice_luma = sps->sps_log2_diff_max_tt_min_qt_intra_slice_luma;


            phs->ph_log2_diff_min_qt_min_cb_intra_slice_chroma = sps->sps_log2_diff_min_qt_min_cb_intra_slice_chroma;
            phs->ph_max_mtt_hierarchy_depth_intra_slice_chroma = sps->sps_max_mtt_hierarchy_depth_intra_slice_chroma;
            phs->ph_log2_diff_max_bt_min_qt_intra_slice_chroma = sps->sps_log2_diff_max_bt_min_qt_intra_slice_chroma;
            phs->ph_log2_diff_max_tt_min_qt_intra_slice_chroma = sps->sps_log2_diff_max_tt_min_qt_intra_slice_chroma;

        }

        if( pps->pps_cu_qp_delta_enabled_flag ){
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_qp_delta_subdiv_intra_slice));
          DLOG(INFO) << "## ph_cu_qp_delta_subdiv_intra_slice : " << phs->ph_cu_qp_delta_subdiv_intra_slice;

        } else {
          phs->ph_cu_qp_delta_subdiv_intra_slice = 0;
        }

        if( pps->pps_cu_chroma_qp_offset_list_enabled_flag ){
          TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_chroma_qp_offset_subdiv_intra_slice));
          DLOG(INFO) << "## ph_cu_chroma_qp_offset_subdiv_intra_slice : " << phs->ph_cu_chroma_qp_offset_subdiv_intra_slice;

        } else {
          phs->ph_cu_chroma_qp_offset_subdiv_intra_slice = 0;
        }
    }



    if( phs->ph_inter_slice_allowed_flag ) {
          if( phs->ph_partition_constraints_override_flag ) {
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_min_qt_min_cb_inter_slice));
            DLOG(INFO) << "## ph_log2_diff_min_qt_min_cb_inter_slice : " << phs->ph_log2_diff_min_qt_min_cb_inter_slice;

            TRUE_OR_RETURN(br->ReadUE(&phs->ph_max_mtt_hierarchy_depth_inter_slice));
            DLOG(INFO) << "## ph_max_mtt_hierarchy_depth_inter_slice : " << phs->ph_max_mtt_hierarchy_depth_inter_slice;

            if(phs->ph_max_mtt_hierarchy_depth_inter_slice != 0){
              TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_bt_min_qt_inter_slice));
              DLOG(INFO) << "## ph_log2_diff_max_bt_min_qt_inter_slice : " << phs->ph_log2_diff_max_bt_min_qt_inter_slice;

              TRUE_OR_RETURN(br->ReadUE(&phs->ph_log2_diff_max_tt_min_qt_inter_slice));
              DLOG(INFO) << "## ph_log2_diff_max_tt_min_qt_inter_slice : " << phs->ph_log2_diff_max_tt_min_qt_inter_slice;

            }
          } else {
            phs->ph_log2_diff_min_qt_min_cb_inter_slice = sps->sps_log2_diff_min_qt_min_cb_inter_slice;
            phs->ph_max_mtt_hierarchy_depth_inter_slice = sps->sps_max_mtt_hierarchy_depth_inter_slice;
            phs->ph_log2_diff_max_bt_min_qt_inter_slice = sps->sps_log2_diff_max_bt_min_qt_inter_slice;
            phs->ph_log2_diff_max_tt_min_qt_inter_slice = sps->sps_log2_diff_max_tt_min_qt_inter_slice;
          }

          if(pps->pps_cu_qp_delta_enabled_flag){
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_qp_delta_subdiv_inter_slice));
            DLOG(INFO) << "## ph_cu_qp_delta_subdiv_inter_slice : " << phs->ph_cu_qp_delta_subdiv_inter_slice;

          } else {
            phs->ph_cu_qp_delta_subdiv_inter_slice = 0;
          }

          if(pps->pps_cu_chroma_qp_offset_list_enabled_flag){
            TRUE_OR_RETURN(br->ReadUE(&phs->ph_cu_chroma_qp_offset_subdiv_inter_slice));
            DLOG(INFO) << "## ph_cu_chroma_qp_offset_subdiv_inter_slice : " << phs->ph_cu_chroma_qp_offset_subdiv_inter_slice;

          } else {
            phs->ph_cu_chroma_qp_offset_subdiv_inter_slice = 0;
          }

          if(sps->sps_temporal_mvp_enabled_flag){
              TRUE_OR_RETURN(br->ReadBool(&phs->ph_temporal_mvp_enabled_flag));
              DLOG(INFO) << "## ph_temporal_mvp_enabled_flag : " << ( phs->ph_temporal_mvp_enabled_flag ? "1" : "0");


              if( phs->ph_temporal_mvp_enabled_flag && pps->pps_rpl_info_in_ph_flag && phs->rpl.has_value() ) {

                /******* section check before processing  */
                if(!phs->rpl.has_value()) {
                    DLOG(ERROR) << "rpl not initialized in picture header";
                    return kInvalidStream;
                }

                if(!phs->rpl->reference_pic_list.has_value()) {
                    DLOG(ERROR) << "reference_pic_list not initialized";
                    return kInvalidStream;
                }
                auto& rpl_ref = phs->rpl->reference_pic_list.value();
                if(1 >= (int)rpl_ref.num_ref_entries.size() ||  phs->rpl->RplsIdx[1] >= (int)rpl_ref.num_ref_entries[1].size()) {
                    DLOG(ERROR) << "Invalid reference picture list dimensions";
                    return kInvalidStream;
                }

                int num_ref_entries_l1 = (int)rpl_ref.num_ref_entries[1][phs->rpl->RplsIdx[1]];




                /******************************************************* */



              /*  if( phs->rpl.has_value() && phs->rpl->RplsIdx[1] >= 0 &&
                  phs->rpl->num_ref_entries[ 1 ][ phs->rpl->RplsIdx[ 1 ] ] > 0 ){ */
                if(num_ref_entries_l1 > 0) {

                      TRUE_OR_RETURN(br->ReadBool(&phs->ph_collocated_from_l0_flag));
                      DLOG(INFO) << "## ph_collocated_from_l0_flag : " << ( phs->ph_collocated_from_l0_flag ? "1" : "0");

                } else {
                    phs->ph_collocated_from_l0_flag = true;
                }


                // check for L0

                if(0 >= (int)rpl_ref.num_ref_entries.size() || phs->rpl->RplsIdx[0] >= (int)rpl_ref.num_ref_entries[0].size()) {
                        DLOG(ERROR) << "Invalid L0 reference picture list dimensions";
                        return kInvalidStream;
                }

              int num_ref_entries_l0 = (int)rpl_ref.num_ref_entries[0][phs->rpl->RplsIdx[0]];

              if((phs->ph_collocated_from_l0_flag && num_ref_entries_l0 > 1) || (!phs->ph_collocated_from_l0_flag && num_ref_entries_l1 > 1)) {
                TRUE_OR_RETURN(br->ReadUE(&phs->ph_collocated_ref_idx));
                DLOG(INFO) << "## ph_collocated_ref_idx : " << phs->ph_collocated_ref_idx;
              } else {
                phs->ph_collocated_ref_idx = 0;
              }

              }
          }
          if(sps->sps_mmvd_fullpel_only_enabled_flag){
            TRUE_OR_RETURN(br->ReadBool(&phs->ph_mmvd_fullpel_only_flag));
            DLOG(INFO) << "## ph_mmvd_fullpel_only_flag : " << ( phs->ph_mmvd_fullpel_only_flag ? "1" : "0");

          } else {
            phs->ph_mmvd_fullpel_only_flag = false;

          }
          bool presenceFlag = false;

          if( !pps->pps_rpl_info_in_ph_flag ){
            presenceFlag = true;
          } else if(phs->rpl.has_value() && phs->rpl->reference_pic_list.has_value()) {

            auto& rpl_ref = phs->rpl->reference_pic_list.value();

            // check dims
            if(1 < rpl_ref.num_ref_entries.size() &&
              static_cast<size_t>(phs->rpl->RplsIdx[1]) < rpl_ref.num_ref_entries[1].size()) {

                if(rpl_ref.num_ref_entries[1][phs->rpl->RplsIdx[1]] > 0) {
                    presenceFlag = true;
                }
            }
          }

          if(!presenceFlag){
            DLOG(INFO) << "## Skip ph_mvd_l1_zero_flag etc.. because presenceFlag is false ";
            phs->ph_mvd_l1_zero_flag = true;
          }

       // }// TEST  TRY FIX 
      //}


      if( presenceFlag ) {
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_mvd_l1_zero_flag));
        DLOG(INFO) << "## ph_mvd_l1_zero_flag : " << ( phs->ph_mvd_l1_zero_flag ? "1" : "0");


        if(sps->sps_bdof_control_present_in_ph_flag){
          TRUE_OR_RETURN(br->ReadBool(&phs->ph_bdof_disabled_flag));
          DLOG(INFO) << "## ph_bdof_disabled_flag : " << ( phs->ph_bdof_disabled_flag ? "1" : "0");

        } else {
                if (!sps->sps_bdof_control_present_in_ph_flag){
                  phs->ph_bdof_disabled_flag = 1-sps->sps_bdof_enabled_flag;
                } else {
                  phs->ph_bdof_disabled_flag = true;
                }
        }

        if(sps->sps_dmvr_control_present_in_ph_flag){
          TRUE_OR_RETURN(br->ReadBool(&phs->ph_dmvr_disabled_flag));
          DLOG(INFO) << "## ph_dmvr_disabled_flag : " << ( phs->ph_dmvr_disabled_flag ? "1" : "0");
        } else {
          if( !sps->sps_dmvr_control_present_in_ph_flag){
            phs->ph_dmvr_disabled_flag = 1 - sps->sps_dmvr_enabled_flag;
          } else {
            phs->ph_dmvr_disabled_flag = true;
        }

      } 
    }


      if(sps->sps_prof_control_present_in_ph_flag){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_prof_disabled_flag));
        DLOG(INFO) << "## ph_prof_disabled_flag : " << ( phs->ph_prof_disabled_flag ? "1" : "0");

      } else {
        phs->ph_prof_disabled_flag = !sps->sps_affine_prof_enabled_flag;
      }

      /****************** log debug *********************** */
      DLOG(INFO) << "DEBUG: pps->pps_rpl_info_in_ph_flag = " << pps->pps_rpl_info_in_ph_flag;
      DLOG(INFO) << "DEBUG: phs->rpl.has_value() = " << phs->rpl.has_value();
      if(phs->rpl.has_value()) {
         DLOG(INFO) << "DEBUG: phs->rpl->reference_pic_list.has_value() = "
               << phs->rpl->reference_pic_list.has_value();
        if(phs->rpl->reference_pic_list.has_value()) {
            auto& rpl = phs->rpl->reference_pic_list.value();
            DLOG(INFO) << "DEBUG: num_ref_entries.size() = " << rpl.num_ref_entries.size();
            if(rpl.num_ref_entries.size() > 1) {
                DLOG(INFO) << "DEBUG: num_ref_entries[1].size() = " << rpl.num_ref_entries[1].size();
            }
        }
      }

      /******************************************** */


      if( ( pps->pps_weighted_pred_flag || pps->pps_weighted_bipred_flag ) && pps->pps_wp_info_in_ph_flag ){
        phs->p_pwt.emplace();

        //PredWeightTable(sps, pps, br, phs->rpl, phs->p_pwt);
        if (phs->rpl.has_value() && phs->p_pwt) {
           TRUE_OR_RETURN(PredWeightTable(*sps, *pps, br, &phs->rpl.value(), &phs->p_pwt.value(), phs->rpl->reference_pic_list->NumRefIdxActive[0]));

        }

      }
    }
          if( pps->pps_qp_delta_info_in_ph_flag ){
            TRUE_OR_RETURN(br->ReadSE(&phs->ph_qp_delta));
            DLOG(INFO) << "## >ph_qp_delta : " << phs->ph_qp_delta;

          }
          if(sps->sps_joint_cbcr_enabled_flag){
        TRUE_OR_RETURN(br->ReadBool(&phs->ph_joint_cbcr_sign_flag));
        DLOG(INFO) << "## ph_joint_cbcr_sign_flag : " << ( phs->ph_joint_cbcr_sign_flag ? "1" : "0");

          } else {
            phs->ph_joint_cbcr_sign_flag = false;
          }
          
          if( sps->sps_sao_enabled_flag && pps->pps_sao_info_in_ph_flag){
            TRUE_OR_RETURN(br->ReadBool(&phs->ph_sao_luma_enabled_flag));
            DLOG(INFO) << "## ph_sao_luma_enabled_flag : " << ( phs->ph_sao_luma_enabled_flag ? "1" : "0");

            if(sps->sps_chroma_format_idc != 0){
              TRUE_OR_RETURN(br->ReadBool(&phs->ph_sao_chroma_enabled_flag));
              DLOG(INFO) << "## ph_sao_chroma_enabled_flag : " << ( phs->ph_sao_chroma_enabled_flag ? "1" : "0");

            } else {
              phs->ph_sao_chroma_enabled_flag = false;
            }
          } else {
            phs->ph_sao_luma_enabled_flag = false;
            phs->ph_sao_chroma_enabled_flag = false;
          }

          if(pps->pps_dbf_info_in_ph_flag){
              TRUE_OR_RETURN(br->ReadBool(&phs->ph_deblocking_params_present_flag));
              DLOG(INFO) << "## ph_deblocking_params_present_flag : " << ( phs->ph_deblocking_params_present_flag ? "1" : "0");

            if(phs->ph_deblocking_params_present_flag && pps){
              if(!pps->pps_deblocking_filter_disabled_flag){
                  TRUE_OR_RETURN(br->ReadBool(&phs->ph_deblocking_filter_disabled_flag));
                  DLOG(INFO) << "## ph_deblocking_filter_disabled_flag : " << ( phs->ph_deblocking_filter_disabled_flag ? "1" : "0");

                if(!phs->ph_deblocking_filter_disabled_flag){
                    TRUE_OR_RETURN(br->ReadSE(&phs->ph_luma_beta_offset_div2));
                    DLOG(INFO) << "## ph_luma_beta_offset_div2 : " << phs->ph_luma_beta_offset_div2;

                    TRUE_OR_RETURN(br->ReadSE(&phs->ph_luma_tc_offset_div2));
                    DLOG(INFO) << "## ph_luma_tc_offset_div2 : " << phs->ph_luma_tc_offset_div2;

                  if(pps->pps_chroma_tool_offsets_present_flag){
                    TRUE_OR_RETURN(br->ReadSE(&phs->ph_cb_beta_offset_div2));
                    DLOG(INFO) << "## ph_cb_beta_offset_div2 : " << phs->ph_cb_beta_offset_div2;

                    TRUE_OR_RETURN(br->ReadSE(&phs->ph_cb_tc_offset_div2));
                    DLOG(INFO) << "## ph_cb_tc_offset_div2 : " << phs->ph_cb_tc_offset_div2;

                    TRUE_OR_RETURN(br->ReadSE(&phs->ph_cr_beta_offset_div2));
                    DLOG(INFO) << "## ph_cr_beta_offset_div2 : " << phs->ph_cr_beta_offset_div2;

                    TRUE_OR_RETURN(br->ReadSE(&phs->ph_cr_tc_offset_div2));
                    DLOG(INFO) << "## ph_cr_tc_offset_div2 : " << phs->ph_cr_tc_offset_div2;

                  }
              } else {
                phs->ph_deblocking_filter_disabled_flag = false;
              }

            }
          } else {
            phs->ph_deblocking_filter_disabled_flag = pps->pps_deblocking_filter_disabled_flag;
            if(phs->ph_deblocking_filter_disabled_flag){
              // Default values when deblocking is disabled
              phs->ph_luma_beta_offset_div2 = pps->pps_luma_beta_offset_div2;
              phs->ph_luma_tc_offset_div2 = pps->pps_luma_tc_offset_div2;
              phs->ph_cb_beta_offset_div2 = pps->pps_cb_beta_offset_div2;
              phs->ph_cb_tc_offset_div2 = pps->pps_cb_tc_offset_div2;
              phs->ph_cr_beta_offset_div2 = pps->pps_cr_beta_offset_div2;
              phs->ph_cr_tc_offset_div2 = pps->pps_cr_tc_offset_div2;
            }

          }

        }




    if( pps->pps_picture_header_extension_present_flag ) {
      int tmp_ph_extension_length;
      TRUE_OR_RETURN(br->ReadUE(&tmp_ph_extension_length));
      phs->ph_extension_length = tmp_ph_extension_length;
      DLOG(INFO) << "## ph_extension_length : " << tmp_ph_extension_length;


      int max_ph_extension_length = phs->ph_extension_length;
      //int tmp_ph_extension_data_byte = 0;
      for( int i = 0; i < max_ph_extension_length; i++){
        int tmp_ph_extension_data_byte = 0;
        TRUE_OR_RETURN(br->ReadUE(&tmp_ph_extension_data_byte));
        phs->ph_extension_data_byte.push_back(tmp_ph_extension_data_byte);
        DLOG(INFO) << "## ph_extension_data_byte : " << tmp_ph_extension_data_byte;

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
/*
uint32_t H266Parser::GetMaxLayers(int vps_id) {
  const H266Vps* vps = GetVps(vps_id);
  return vps ? (vps->vps_max_layers_minus1 + 1) : 1;
} */

uint32_t H266Parser::GetMaxLayers(int vps_id) {
  // ✅ Version non-const  //)
  H266Vps* vps = GetVps(vps_id);
  return vps ? (vps->vps_max_layers_minus1 + 1) : 1;
}


bool H266Parser::IsLayerIndependent(int vps_id, uint32_t layer_id) {
  //const
  H266Vps* vps = GetVps(vps_id);
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




/******************************************************************* */
#if 0

const H266Pps* H266Parser::GetPps(int pps_id) {
  return active_ppses_[pps_id].get();
}

H266Pps* H266Parser::GetPps(int pps_id) {
  return active_ppses_[pps_id].get();
}

const H266Sps* H266Parser::GetSps(int sps_id) {
  return active_spses_[sps_id].get();
}

H266Sps* H266Parser::GetSps(int sps_id) {
  return active_spses_[sps_id].get();
}


const H266Vps* H266Parser::GetVps(int vps_id) {
  return active_vpses_[vps_id].get();
  //auto it = active_vpses_.find(vps_id);
  //return it != active_vpses_.end() ? it->second.get() : nullptr;
}









// Function to obtain the first SPS for a given VPS
std::vector<const H266Pps*> H266Parser::GetPpsForSps(int sps_id) {
  std::vector<const H266Pps*> result;

  for (const auto& pps_pair : active_ppses_) {
    if (pps_pair.second->pps_seq_parameter_set_id == sps_id) {
      result.push_back(pps_pair.second.get());
    }
  }

  return result;
}
// Function to obtain all PPS for a given SPS
std::vector<const H266Sps*> H266Parser::GetSpsForVps(int vps_id) {
  std::vector<const H266Sps*> result;

  for (const auto& sps_pair : active_spses_) {
    if (sps_pair.second->sps_video_parameter_set_id == vps_id) {
      result.push_back(sps_pair.second.get());
    }
  }

  return result;
}

std::vector<const H266Pps*> H266Parser::GetPpsFromSps(int sps_id) {
  std::vector<const H266Pps*> result;

  for (const auto& pps_pair : active_ppses_) {
    if (pps_pair.second->pps_seq_parameter_set_id == sps_id) {
      result.push_back(pps_pair.second.get());
    }
  }

  return result;
}

std::vector<const H266Pps*> H266Parser::GetPpsFromVps(int vps_id) {
  std::vector<const H266Pps*> result;

  for (const auto& pps_pair : active_ppses_) {
    const H266Pps* pps = pps_pair.second.get();
    auto sps_it = active_spses_.find(pps->pps_seq_parameter_set_id);
    if (sps_it != active_spses_.end() &&
        sps_it->second->sps_video_parameter_set_id == vps_id) {
      result.push_back(pps);
    }
  }

  return result;
}

// Function to obtain all SPS for a given VPS
std::vector<const H266Sps*> H266Parser::GetSpsFromVps(int vps_id) {
  std::vector<const H266Sps*> result;

  for (const auto& sps_pair : active_spses_) {
    if (sps_pair.second->sps_video_parameter_set_id == vps_id) {
      result.push_back(sps_pair.second.get());
    }
  }

  return result;
}

std::vector<const H266Vps*> H266Parser::GetVpsFromSps(int sps_id) {
  std::vector<const H266Vps*> result;

  auto sps_it = active_spses_.find(sps_id);
  if (sps_it != active_spses_.end()) {
    int vps_id = sps_it->second->sps_video_parameter_set_id;
    auto vps_it = active_vpses_.find(vps_id);
    if (vps_it != active_vpses_.end()) {
      result.push_back(vps_it->second.get());
    }
  }

  return result;
}


// Function to obtain the first PPS for a given SPS
const H266Sps* H266Parser::GetFirstSpsForVps(int vps_id) {
  for (const auto& sps_pair : active_spses_) {
    if (sps_pair.second->sps_video_parameter_set_id == vps_id) {
      return sps_pair.second.get();
    }
  }
  return nullptr;
}


const H266Pps* H266Parser::GetFirstPpsForSps(int sps_id) {
  for (const auto& pps_pair : active_ppses_) {
    if (pps_pair.second->pps_seq_parameter_set_id == sps_id) {
      return pps_pair.second.get();
    }
  }
  return nullptr;
}

const H266Pps* H266Parser::GetFirstPpsFromVps(int vps_id) {
  for (const auto& pps_pair : active_ppses_) {
    const H266Pps* pps = pps_pair.second.get();
    auto sps_it = active_spses_.find(pps->pps_seq_parameter_set_id);
    if (sps_it != active_spses_.end() &&
        sps_it->second->sps_video_parameter_set_id == vps_id) {
      return pps;
    }
  }
  return nullptr;
}

const H266Sps* H266Parser::GetFirstSpsFromPps(int pps_id) {
  auto pps_it = active_ppses_.find(pps_id);
  if (pps_it != active_ppses_.end()) {
    int sps_id = pps_it->second->pps_seq_parameter_set_id;
    auto sps_it = active_spses_.find(sps_id);
    if (sps_it != active_spses_.end()) {
      return sps_it->second.get();
    }
  }
  return nullptr;
}

const H266Vps* H266Parser::GetFirstVpsFromPps(int pps_id) {
  auto pps_it = active_ppses_.find(pps_id);
  if (pps_it != active_ppses_.end()) {
    int sps_id = pps_it->second->pps_seq_parameter_set_id;
    auto sps_it = active_spses_.find(sps_id);
    if (sps_it != active_spses_.end()) {
      int vps_id = sps_it->second->sps_video_parameter_set_id;
      auto vps_it = active_vpses_.find(vps_id);
      if (vps_it != active_vpses_.end()) {
        return vps_it->second.get();
      }
    }
  }
  return nullptr;
}

const H266Vps* H266Parser::GetFirstVpsFromSps(int sps_id) {
  auto sps_it = active_spses_.find(sps_id);
  if (sps_it != active_spses_.end()) {
    int vps_id = sps_it->second->sps_video_parameter_set_id;
    auto vps_it = active_vpses_.find(vps_id);
    if (vps_it != active_vpses_.end()) {
      return vps_it->second.get();
    }
  }
  return nullptr;
}


const H266Vps* H266Parser::GetFirstVps() {
  if (active_vpses_.empty()) {
    LOG(INFO) << "GetFirstVps  vps id not available";
    return nullptr;
  }
  return active_vpses_.begin()->second.get();
}

const H266Sps* H266Parser::GetFirstSps() {
  if (active_spses_.empty()) {
        LOG(INFO) << "GetFirstSps  sps id not available";

    return nullptr;
  }
  return active_spses_.begin()->second.get();
}

const H266Pps* H266Parser::GetFirstPps() {
  if (active_ppses_.empty()) {
    LOG(INFO) << "GetFirstPps  pps id not available";
    return nullptr;
  }
  return active_ppses_.begin()->second.get();
}
#else

bool H266Parser::HasVps(int vps_id) const {
  return active_vpses_.find(vps_id) != active_vpses_.end();
}

bool H266Parser::HasSps(int sps_id) const {
    return active_spses_.find(sps_id) != active_spses_.end();
}

bool H266Parser::HasPps(int pps_id) const {
  return active_ppses_.find(pps_id) != active_ppses_.end();
}

bool H266Parser::HasAps(int aps_id) const {
  return active_apses_.find(aps_id) != active_apses_.end();
}


// ==================== GETTERS CONST ====================
const H266Pps* H266Parser::GetPps(int pps_id) const {
  auto it = active_ppses_.find(pps_id);
  return it != active_ppses_.end() ? it->second.get() : nullptr;
}

const H266Sps* H266Parser::GetSps(int sps_id) const {
  auto it = active_spses_.find(sps_id);
  return it != active_spses_.end() ? it->second.get() : nullptr;
}

const H266Vps* H266Parser::GetVps(int vps_id) const {
  auto it = active_vpses_.find(vps_id);
  return it != active_vpses_.end() ? it->second.get() : nullptr;
}

const H266Aps* H266Parser::GetAps(int aps_id) const {
  auto it = active_apses_.find(aps_id);
  return it != active_apses_.end() ? it->second.get() : nullptr;
}

// ==================== GETTERS NON-CONST ====================
H266Pps* H266Parser::GetPps(int pps_id) {
  ////return active_ppses_[pps_id].get();

  //auto it = active_ppses_.find(pps_id);
  //return it != active_ppses_.end() ? it->second.get() : nullptr;
  /* auto it = active_ppses_.find(pps_id);
  if (it != active_ppses_.end()) {
    return it->second.get();
  }
  DLOG(WARNING) << "PPS with id " << pps_id << " not found";
  return nullptr; */

   DLOG(INFO) << "GetPps requested for id: " << pps_id;

  if (active_ppses_.empty()) {
    DLOG(ERROR) << "No PPS available in parser";
    return nullptr;
  }

  auto it = active_ppses_.find(pps_id);
  if (it == active_ppses_.end()) {
    DLOG(ERROR) << "PPS " << pps_id << " not found. Available:";
    for (const auto& pair : active_ppses_) {
      DLOG(ERROR) << "  - PPS id " << pair.first;
    }
    return nullptr;
  }

  if (!it->second) {
    DLOG(ERROR) << "PPS " << pps_id << " found but pointer is null!";
    return nullptr;
  }

  DLOG(INFO) << "Successfully retrieved PPS " << pps_id;
  return it->second.get();




}

H266Sps* H266Parser::GetSps(int sps_id) {
  //auto it = active_spses_.find(sps_id);
  //return it != active_spses_.end() ? it->second.get() : nullptr;
  ////return active_spses_[sps_id].get();
  auto it = active_spses_.find(sps_id);
  if (it != active_spses_.end()) {
    return it->second.get();
  }
  DLOG(WARNING) << "SPS with id " << sps_id << " not found";
  return nullptr;

}

H266Vps* H266Parser::GetVps(int vps_id) {
  //auto it = active_vpses_.find(vps_id);
  //return it != active_vpses_.end() ? it->second.get() : nullptr;
  ////return active_vpses_[vps_id].get();
  auto it = active_vpses_.find(vps_id);
  if (it != active_vpses_.end()) {
    return it->second.get();
  }
  DLOG(WARNING) << "VPS with id " << vps_id << " not found";
  return nullptr;
}

// ==================== FIRST GETTERS ====================
H266Vps* H266Parser::GetFirstVps() {
  if (active_vpses_.empty()) {
    LOG(INFO) << "GetFirstVps: vps id not available";
    return nullptr;
  }
  return active_vpses_.begin()->second.get();
}

H266Sps* H266Parser::GetFirstSps() {
  if (active_spses_.empty()) {
    LOG(INFO) << "GetFirstSps: sps id not available";
    return nullptr;
  }
  return active_spses_.begin()->second.get();
}

H266Pps* H266Parser::GetFirstPps() {
  if (active_ppses_.empty()) {
    LOG(INFO) << "GetFirstPps: pps id not available";
    return nullptr;
  }
  return active_ppses_.begin()->second.get();
}

H266Sps* H266Parser::GetFirstSpsForVps(int vps_id) {
  for (auto& sps_pair : active_spses_) {
    if (sps_pair.second->sps_video_parameter_set_id == vps_id) {
      return sps_pair.second.get();
    }
  }
  return nullptr;
}

H266Pps* H266Parser::GetFirstPpsForSps(int sps_id) {
  for (auto& pps_pair : active_ppses_) {
    if (pps_pair.second->pps_seq_parameter_set_id == sps_id) {
      return pps_pair.second.get();
    }
  }
  return nullptr;
}

H266Pps* H266Parser::GetFirstPpsFromVps(int vps_id) {
  for (auto& pps_pair : active_ppses_) {
    H266Pps* pps = pps_pair.second.get();
    auto sps_it = active_spses_.find(pps->pps_seq_parameter_set_id);
    if (sps_it != active_spses_.end() &&
        sps_it->second->sps_video_parameter_set_id == vps_id) {
      return pps;
    }
  }
  return nullptr;
}

H266Sps* H266Parser::GetFirstSpsFromPps(int pps_id) {
  auto pps_it = active_ppses_.find(pps_id);
  if (pps_it != active_ppses_.end()) {
    int sps_id = pps_it->second->pps_seq_parameter_set_id;
    auto sps_it = active_spses_.find(sps_id);
    if (sps_it != active_spses_.end()) {
      return sps_it->second.get();
    }
  }
  return nullptr;
}

H266Vps* H266Parser::GetFirstVpsFromPps(int pps_id) {
  auto pps_it = active_ppses_.find(pps_id);
  if (pps_it != active_ppses_.end()) {
    int sps_id = pps_it->second->pps_seq_parameter_set_id;
    auto sps_it = active_spses_.find(sps_id);
    if (sps_it != active_spses_.end()) {
      int vps_id = sps_it->second->sps_video_parameter_set_id;
      auto vps_it = active_vpses_.find(vps_id);
      if (vps_it != active_vpses_.end()) {
        return vps_it->second.get();
      }
    }
  }
  return nullptr;
}

H266Vps* H266Parser::GetFirstVpsFromSps(int sps_id) {
  auto sps_it = active_spses_.find(sps_id);
  if (sps_it != active_spses_.end()) {
    int vps_id = sps_it->second->sps_video_parameter_set_id;
    auto vps_it = active_vpses_.find(vps_id);
    if (vps_it != active_vpses_.end()) {
      return vps_it->second.get();
    }
  }
  return nullptr;
}

// ==================== GETTERS POUR COLLECTIONS (const) ====================
std::vector<const H266Pps*> H266Parser::GetPpsForSps(int sps_id) const {
  std::vector<const H266Pps*> result;
  for (const auto& pps_pair : active_ppses_) {
    if (pps_pair.second->pps_seq_parameter_set_id == sps_id) {
      result.push_back(pps_pair.second.get());
    }
  }
  return result;
}

std::vector<const H266Sps*> H266Parser::GetSpsForVps(int vps_id) const {
  std::vector<const H266Sps*> result;
  for (const auto& sps_pair : active_spses_) {
    if (sps_pair.second->sps_video_parameter_set_id == vps_id) {
      result.push_back(sps_pair.second.get());
    }
  }
  return result;
}





#endif
/**************************************************************************** */


/*
const H266Aps* H266Parser::GetAps(int aps_id) const {
  return active_apses_[aps_id].get();
} */
H266Parser::Result H266Parser::GetGeneralTimingHrdParameters(GeneralTimingHrdParameters *time,
                                      H26xBitReader* br){
  LOG(INFO) << "Parsing H.266 GetGeneralTimingHrdParameters in SPS";

  uint32_t num_units, time_scale;
  TRUE_OR_RETURN(br->ReadBits(32, &num_units));
  TRUE_OR_RETURN(br->ReadBits(32, &time_scale));
  time->num_units_in_tick = num_units;
  time->time_scale = time_scale;

  TRUE_OR_RETURN(br->ReadBool(&time->general_nal_hrd_params_present_flag));
  DLOG(INFO) << "## general_nal_hrd_params_present_flag : " << (time->general_nal_hrd_params_present_flag  ? "1" : "0");

  TRUE_OR_RETURN(br->ReadBool(&time->general_vcl_hrd_params_present_flag));
  DLOG(INFO) << "## general_vcl_hrd_params_present_flag: " << (time->general_vcl_hrd_params_present_flag  ? "1" : "0");



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
  //DisplayGeneralTimingHrdParameters(*time);

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
  TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
  //rpls->num_ref_entries[listIdx][rplsIdx].push_back(tmp_num_ref_entries);
  if (rpls->num_ref_entries.size() <= static_cast<size_t>(listIdx)) {
    rpls->num_ref_entries.resize(listIdx + 1);
  }
  if (rpls->num_ref_entries[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
    rpls->num_ref_entries[listIdx].resize(rplsIdx + 1);
  }
  rpls->num_ref_entries[listIdx][rplsIdx] = tmp_num_ref_entries;



  if( sps.sps_long_term_ref_pics_flag && rplsIdx < sps.sps_num_ref_pic_lists[listIdx] && rpls->num_ref_entries[listIdx][rplsIdx] > 0 ){


    if (rpls->ltrp_in_header_flag.size() <= static_cast<size_t>(listIdx)) {
       rpls->ltrp_in_header_flag.resize(listIdx + 1);
    }
    if (rpls->ltrp_in_header_flag[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
      rpls->ltrp_in_header_flag[listIdx].resize(rplsIdx + 1);

    }
      bool tmp_ltrp_in_header_flag = 0;
      TRUE_OR_RETURN(br->ReadBool(&tmp_ltrp_in_header_flag));
      rpls->ltrp_in_header_flag[ listIdx ][ rplsIdx ] = tmp_ltrp_in_header_flag;
  }
  int num_ref_entries = rpls->num_ref_entries[listIdx][rplsIdx];



  for( int i = 0, j = 0; i <  num_ref_entries; i++) {

    if (rpls->inter_layer_ref_pic_flag.size() <= static_cast<size_t>(listIdx)) {
        rpls->inter_layer_ref_pic_flag.resize(listIdx + 1);
    }
    if (rpls->inter_layer_ref_pic_flag[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
      rpls->inter_layer_ref_pic_flag[listIdx].resize(rplsIdx + 1);
    }
    if (rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx].size() <= static_cast<size_t>(i)) {
      rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx].resize(i + 1);
    }



    if( sps.sps_inter_layer_prediction_enabled_flag ){

          bool tmp_inter_layer_ref_pic_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
          rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx][i] = tmp_inter_layer_ref_pic_flag;
    }






    if( !rpls->inter_layer_ref_pic_flag[listIdx][rplsIdx][i] ) {

       // Resize st_ref_pic_flag vector
      if (rpls->st_ref_pic_flag.size() <= static_cast<size_t>(listIdx)) {
        rpls->st_ref_pic_flag.resize(listIdx + 1);
      }
      if (rpls->st_ref_pic_flag[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
        rpls->st_ref_pic_flag[listIdx].resize(rplsIdx + 1);
      }
      if (rpls->st_ref_pic_flag[listIdx][rplsIdx].size() <= static_cast<size_t>(i)) {
        rpls->st_ref_pic_flag[listIdx][rplsIdx].resize(i + 1);
      }





      if( sps.sps_long_term_ref_pics_flag ){
        bool tmp_st_ref_pic_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_st_ref_pic_flag));
        rpls->st_ref_pic_flag[listIdx][rplsIdx][i] =tmp_st_ref_pic_flag;
      }
      if( rpls->st_ref_pic_flag[listIdx][rplsIdx][i]) {

        //TRUE_OR_RETURN(br->ReadUE(&tmp_st_ref_pic_flag));
        int tmp_abs_delta_poc_st = 0;
        TRUE_OR_RETURN(br->ReadUE(&tmp_abs_delta_poc_st));
        rpls->abs_delta_poc_st[listIdx][rplsIdx][i] = tmp_abs_delta_poc_st;
        //compute AbsDeltaPocSt
        int abs_delta_poc_st_value = 0;

        if (AbsDeltaPocSt.size() <= static_cast<size_t>(listIdx)) {
            AbsDeltaPocSt.resize(listIdx + 1);
        }
        if (AbsDeltaPocSt[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
           AbsDeltaPocSt[listIdx].resize(rplsIdx + 1);
        }
        if (AbsDeltaPocSt[listIdx][rplsIdx].size() <= static_cast<size_t>(i)) {
          AbsDeltaPocSt[listIdx][rplsIdx].resize(i + 1);
        }



        if( ( sps.sps_weighted_pred_flag || sps.sps_weighted_bipred_flag ) && i != 0 )
        {
          AbsDeltaPocSt[listIdx][rplsIdx][i] = rpls->abs_delta_poc_st[listIdx][rplsIdx][i];
        }
        else{

          int abs_delta_poc_st_value = rpls->abs_delta_poc_st[listIdx][rplsIdx].at(i) + 1;
           AbsDeltaPocSt[listIdx][rplsIdx][i] = std::vector<int>{abs_delta_poc_st_value};
           //abs_delta_poc_st_value;    NOT SURE

        }
        //if( AbsDeltaPocSt[listIdx][rplsIdx].at(i) > 0 )
        if( abs_delta_poc_st_value > 0 )
        {
          bool tmp_strp_entry_sign_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
          rpls->strp_entry_sign_flag[ listIdx ][ rplsIdx ][ i ] = tmp_strp_entry_sign_flag;
        }

      }

      else if( !rpls->ltrp_in_header_flag[listIdx][rplsIdx] ){
        //The length of the rpls_poc_lsb_lt[ listIdx ][ rplsIdx ][ i ] syntax element is sps_log2_max_pic_order_cnt_lsb_minus4 + 4 bits
        int bit_read = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
        int tmp_rpls_poc_lsb_lt = 0;
        TRUE_OR_RETURN(br->ReadBits(bit_read,&tmp_rpls_poc_lsb_lt));
        rpls->rpls_poc_lsb_lt[ listIdx ][ rplsIdx ][ j++ ] = tmp_rpls_poc_lsb_lt;
      }

    }else{
      int tmp_ilrp_idx = 0;
      TRUE_OR_RETURN(br->ReadUE(&tmp_ilrp_idx));
      rpls->ilrp_idx[listIdx][rplsIdx][i] = tmp_ilrp_idx;
    }
  }
  return kOk;
}
#endif
H266Parser::Result H266Parser::PredWeightTable( const H266Sps& sps, const H266Pps& pps,
                          H26xBitReader* br,
                          H266ReferencePicList *rpl,
                          H266PredWeightTable *pwt,
                          int numweightsw0){
    LOG(INFO) << "Parsing H.266 Pred Weight Table ";

    // todo add parameter
    // get num_ref_entries  et  RplsIdx  from H266ReferencePicList
    //NumRefIdxActive  equa 139 P157








    TRUE_OR_RETURN(br->ReadUE(&pwt->luma_log2_weight_denom));
    if( sps.sps_chroma_format_idc != 0 ){
      TRUE_OR_RETURN(br->ReadSE(&pwt->delta_chroma_log2_weight_denom));
    }
    if( pps.pps_wp_info_in_ph_flag ){
      TRUE_OR_RETURN(br->ReadUE(&pwt->num_l0_weights));
    }
      /****************************************************************/
    int NumWeightsL0 = 0;
    if( pps.pps_wp_info_in_ph_flag ){
      NumWeightsL0 = pwt->num_l0_weights;
    } else if (!pps.pps_wp_info_in_ph_flag){
      NumWeightsL0 = numweightsw0;//slice_header->NumRefIdxActive[ 0 ]; //  how get it ? param ?
    }
    /****************************************************************/



    //NumWeightsL0 evalaute
    //int NumWeightsL0 = 0
    for(int i = 0; i < NumWeightsL0; i++ ){
      bool tmp_luma_weight_l0_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_luma_weight_l0_flag));
      pwt->luma_weight_l0_flag.push_back(tmp_luma_weight_l0_flag);
    }
    if( sps.sps_chroma_format_idc != 0 ){
      for( int i = 0; i < NumWeightsL0; i++ ){
        bool tmp_chroma_weight_l0_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_chroma_weight_l0_flag));
        pwt->chroma_weight_l0_flag.push_back(tmp_chroma_weight_l0_flag);
      }
    }

    for( int i = 0; i < NumWeightsL0; i++ ){
      if( pwt->luma_weight_l0_flag[i] ) {
        int tmp_delta_luma_weight_l0 = 0;
        int tmp_luma_offset_l0 = 0;
        // int tmp_delta_chroma_weight_l0 = 0;
        // int delta_chroma_offset_l0 = 0;

        TRUE_OR_RETURN(br->ReadSE(&tmp_delta_luma_weight_l0));
        TRUE_OR_RETURN(br->ReadSE(&tmp_luma_offset_l0));

        pwt->delta_luma_weight_l0.push_back(tmp_delta_luma_weight_l0);
        pwt->luma_offset_l0.push_back(tmp_luma_offset_l0);
      }
      if( pwt->chroma_weight_l0_flag[i]){
        for(int j = 0; j < 2; j++ ) {
          int tmp_delta_chroma_weight_l0 = 0;
          int tmp_delta_chroma_offset_l0 = 0;
          TRUE_OR_RETURN(br->ReadSE(&tmp_delta_chroma_weight_l0));
          TRUE_OR_RETURN(br->ReadSE(&tmp_delta_chroma_offset_l0));
          pwt->delta_chroma_weight_l0.push_back(tmp_delta_chroma_weight_l0);
          pwt->delta_chroma_offset_l0.push_back(tmp_delta_chroma_offset_l0);
        }
      }
    }

    //int check_entry = rpl->reference_pic_list->num_ref_entries[1][rpl->RplsIdx[1]];
    /* int check_entry = 0;
    if (rpl->reference_pic_list.has_value()) {
        check_entry = rpl->reference_pic_list->num_ref_entries[1][rpl->RplsIdx[1]];
    } */
    /* int check_entry = 0;
    if (rpl->reference_pic_list.has_value()) {
          auto& rpl_struct = rpl->reference_pic_list.value();
          if (!rpl_struct.is_valid) {
            DVLOG(3) << "no rpl_struct";
          }
          if (1 < rpl_struct.num_ref_entries.size() &&
              static_cast<size_t>(rpl->RplsIdx[1]) < rpl_struct.num_ref_entries[1].size()) {
            check_entry = rpl_struct.num_ref_entries[1][rpl->RplsIdx[1]];
            if (check_entry > 0) {
              DVLOG(3) << "L1 reference entries: " << check_entry;//fot remove warning
            }
           }
    }
     */
    int check_entry = 0;
    if (rpl->reference_pic_list.has_value()) {
          auto& rpl_struct = rpl->reference_pic_list.value();
          // Vérifier que les entrées existent
          if (!rpl_struct.entries.empty()) {
            check_entry = rpl_struct.entries.size();
            if (check_entry > 0) {
              DVLOG(3) << "L1 reference entries: " << check_entry;
            }
          }
    }


    if( pps.pps_weighted_bipred_flag && pps.pps_wp_info_in_ph_flag && rpl->reference_pic_list.has_value() && check_entry > 0){// rpl->reference_pic_list->num_ref_entries[1][rpl->RplsIdx[1]] > 0 ){
      TRUE_OR_RETURN(br->ReadSE(&pwt->num_l1_weights));
    }

    int NumWeightsL1 = 0;
    if( !pps.pps_weighted_bipred_flag || ( pps.pps_wp_info_in_ph_flag && rpl->reference_pic_list->num_ref_entries[1][rpl->RplsIdx[1]] == 0 ) ){
      NumWeightsL1 = 0;
    } else if (pps.pps_wp_info_in_ph_flag){
      NumWeightsL1 = pwt->num_l1_weights;
    }
    else{
      NumWeightsL1 = numweightsw0;//slice_header->NumRefIdxActive[1];
    }

    for( int i = 0; i < NumWeightsL1; i++ ){
      bool tmp_luma_weight_l1_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_luma_weight_l1_flag));
      pwt->luma_weight_l1_flag.push_back(tmp_luma_weight_l1_flag);
    }
    if( sps.sps_chroma_format_idc != 0 ){
      for( int i = 0; i < NumWeightsL1; i++ ){
        bool tmp_chroma_weight_l1_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_chroma_weight_l1_flag));
        pwt->chroma_weight_l1_flag.push_back(tmp_chroma_weight_l1_flag);
      }
    }

    for( int i = 0; i < NumWeightsL1; i++ ){
      if( pwt->luma_weight_l1_flag[i] ) {
         int tmp_delta_luma_weight_l1 = 0;
        int tmp_luma_offset_l1 = 0;
        TRUE_OR_RETURN(br->ReadSE(&tmp_delta_luma_weight_l1));
        TRUE_OR_RETURN(br->ReadSE(&tmp_luma_offset_l1));
        pwt->delta_luma_weight_l1.push_back(tmp_delta_luma_weight_l1);
        pwt->luma_offset_l1.push_back(tmp_luma_offset_l1);
      }
      if( pwt->chroma_weight_l1_flag[i]){
        for(int j = 0; j < 2; j++ ) {
          int tmp_delta_chroma_weight_l1 = 0;
          int delta_chroma_offset_l1 = 0;
          TRUE_OR_RETURN(br->ReadSE(&tmp_delta_chroma_weight_l1));
          TRUE_OR_RETURN(br->ReadSE(&delta_chroma_offset_l1));
          pwt->delta_chroma_weight_l1[i].push_back(tmp_delta_chroma_weight_l1);
          pwt->delta_chroma_offset_l1[i].push_back(delta_chroma_offset_l1);
        }
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
    //in use

    // Initialize vectors to proper size
    rpl->rpl_sps_flag.clear();
    rpl->rpl_idx.clear();
    rpl->NumLtrpEntries.clear();
    rpl->RplsIdx.clear();
    rpl->poc_lsb_lt.clear();
    rpl->delta_poc_msb_cycle_present_flag.clear();
    rpl->delta_poc_msb_cycle_lt.clear();
    // check reference_pic_list is always init
    if(!rpl->reference_pic_list.has_value()) {
        rpl->reference_pic_list = H266ReferencePicListStruct();
        rpl->reference_pic_list->is_valid = false; // Marquer comme invalide initialement
    }


    //H266ReferencePicListStruct rpl_struct;
    //sps_num_ref_pic_lists[i]  o or 1
    // rpls_idx[i] : list index
    H266ReferencePicListStruct rpl_struct;

    for (int i = 0; i < 2; i++) {
        int num_lists = sps.sps_num_ref_pic_lists[i];
        rpl_struct.num_ref_entries.resize(2);
        rpl_struct.ltrp_in_header_flag.resize(2);
        rpl_struct.entries.resize(2);
        rpl_struct.NumRefIdxActive.resize(2, 0);
        rpl_struct.inter_layer_ref_pic_flag.resize(2);
        rpl_struct.st_ref_pic_flag.resize(2);
        rpl_struct.abs_delta_poc_st.resize(2);
        rpl_struct.strp_entry_sign_flag.resize(2);
        rpl_struct.rpls_poc_lsb_lt.resize(2);
        rpl_struct.ilrp_idx.resize(2);
        // Resize for each RPL index
        rpl_struct.num_ref_entries[i].resize(num_lists);
        rpl_struct.ltrp_in_header_flag[i].resize(num_lists);
        rpl_struct.entries[i].resize(num_lists);
        rpl_struct.inter_layer_ref_pic_flag[i].resize(num_lists);
        rpl_struct.st_ref_pic_flag[i].resize(num_lists);
        rpl_struct.abs_delta_poc_st[i].resize(num_lists);
        rpl_struct.strp_entry_sign_flag[i].resize(num_lists);
        rpl_struct.rpls_poc_lsb_lt[i].resize(num_lists);
        rpl_struct.ilrp_idx[i].resize(num_lists);

    }

    for(int i = 0; i < 2; i++) {
        if(sps.sps_num_ref_pic_lists[i] > 0 && (i == 0 || (i == 1 && pps.pps_rpl1_idx_present_flag))) {
            bool tmp_rpl_sps_flag = false;
            TRUE_OR_RETURN(br->ReadBool(&tmp_rpl_sps_flag));
            rpl->rpl_sps_flag.push_back(tmp_rpl_sps_flag);
        } else {

            rpl->rpl_sps_flag.push_back(false);
        }

        if(rpl->rpl_sps_flag[i]) {
            if(sps.sps_num_ref_pic_lists[i] > 1 && (i == 0 || (i == 1 && pps.pps_rpl1_idx_present_flag))) {
                int len_rpl_idx = ceil_log2(sps.sps_num_ref_pic_lists[i]);

                int tmp_rpl_idx = 0;
                TRUE_OR_RETURN(br->ReadBits(len_rpl_idx, &tmp_rpl_idx));
                rpl->rpl_idx.push_back(tmp_rpl_idx);
            } else if (i == 1 && !pps.pps_rpl1_idx_present_flag) {
              if (rpl->rpl_idx.size() > 0) {
                //rpl->rpl_idx[1] = rpl->rpl_idx[0];
                rpl->rpl_idx.push_back(rpl->rpl_idx[0]);

              } else {
                  rpl->rpl_idx.push_back(0);
              }
            } else{
              DLOG(ERROR) << " CAN NOT POPULATE the rpl_idx[i]";
              rpl->rpl_idx.push_back(0); // Add default
            }
            //memcpy(&rpl[i],&rpl_struct[i][rpl->rpl_idx[i]],i,sizeof(rpl[i]));

            /*  else {
                rpl->rpl_idx.push_back(0);
            } */


        } else {

             // Parse reference picture list structure
            DLOG(INFO) << "Ref_Pic_List_Struct i:" << i
                       << " sps_num_ref_pic_lists:" << sps.sps_num_ref_pic_lists[i];

            // Parse each RPL for this list index
            for (int rplsIdx = 0; rplsIdx < sps.sps_num_ref_pic_lists[i]; rplsIdx++) {
                // We need to parse the RPL structure into the appropriate position
                // Create a temporary struct for parsing
                H266ReferencePicListStruct tmp_rpl_struct;

                // Parse this specific RPL
                // Note: We need to know which RPL index we're parsing
                // Ref_Pic_List_Struct might need to be modified to handle 3D structure
                TRUE_OR_RETURN(Ref_Pic_List_Struct(i, rplsIdx, sps, br, &tmp_rpl_struct));

                // Copy the parsed data into the appropriate position
                // This assumes Ref_Pic_List_Struct fills the first element of each vector
                if (rplsIdx < static_cast<int>(rpl_struct.num_ref_entries[i].size())) {
                    if (!tmp_rpl_struct.num_ref_entries.empty() &&
                        !tmp_rpl_struct.num_ref_entries[0].empty()) {
                        rpl_struct.num_ref_entries[i][rplsIdx] = tmp_rpl_struct.num_ref_entries[0][0];
                    }

                    // Copy entries if available
                    if (!tmp_rpl_struct.entries.empty() &&
                        !tmp_rpl_struct.entries[0].empty()) {
                        rpl_struct.entries[i][rplsIdx] = tmp_rpl_struct.entries[0][0];
                    }

                    // Copy other fields similarly...
                }
            }
            // Store the parsed structure
            if(!rpl->reference_pic_list.has_value()) {
                rpl->reference_pic_list = rpl_struct;
            }

        }
    }

    // Initialize NumLtrpEntries and RplsIdx
    rpl->NumLtrpEntries.resize(2);
    rpl->RplsIdx.resize(2);
    /*
    for(int listIdx = 0; listIdx < 2; listIdx++) {
        rpl->RplsIdx[listIdx] = rpl->rpl_idx.size() > static_cast<size_t>(listIdx) ?
                               rpl->rpl_idx[listIdx] : 0;

        // Calculate NumLtrpEntries from entries
        if(rpl->reference_pic_list.has_value()) {
            const auto& rpl_struct = rpl->reference_pic_list.value();
            int numLtrp = 0;

            // Count LTRP entries
            for(const auto& entry : rpl_struct.entries) {
                if(!entry.inter_layer_ref_pic_flag && !entry.st_ref_pic_flag) {
                    numLtrp++;
                }
            }

            rpl->NumLtrpEntries[listIdx].resize(sps.sps_num_ref_pic_lists[listIdx]);
            if(static_cast<size_t>(rpl->RplsIdx[listIdx]) < rpl->NumLtrpEntries[listIdx].size()) {
                rpl->NumLtrpEntries[listIdx][rpl->RplsIdx[listIdx]] = numLtrp;
            }
        }
    } */
    for (int listIdx = 0; listIdx < 2; listIdx++) {
        rpl->RplsIdx[listIdx] = rpl->rpl_idx.size() > static_cast<size_t>(listIdx) ?
                                rpl->rpl_idx[listIdx] : 0;

        // Calculate NumLtrpEntries from entries
        if (rpl->reference_pic_list.has_value()) {
            const auto& rpl_struct = rpl->reference_pic_list.value();

            // Resize NumLtrpEntries for this list
            rpl->NumLtrpEntries[listIdx].resize(sps.sps_num_ref_pic_lists[listIdx]);

            int rplsIdx = rpl->RplsIdx[listIdx];

            // Check bounds
            if (rpl_struct.entries.size() > static_cast<size_t>(listIdx) &&
                rpl_struct.entries[listIdx].size() > static_cast<size_t>(rplsIdx)) {

                int numLtrp = 0;

                // Count LTRP entries FOR THIS entries[listIdx][rplsIdx]
                for (const auto& entry : rpl_struct.entries[listIdx][rplsIdx]) {
                    if (!entry.inter_layer_ref_pic_flag && !entry.st_ref_pic_flag) {
                        numLtrp++;
                    }
                }

                // Stocker le résultat
                if (static_cast<size_t>(rplsIdx) < rpl->NumLtrpEntries[listIdx].size()) {
                    rpl->NumLtrpEntries[listIdx][rplsIdx] = numLtrp;
                }
            }
        }
    }

    // Parse additional LTRP information
    //bool check_delta_poc_msb_cycle_present_flag = false;

    /* for(int i = 0; i < 2; i++) {
        if(!rpl->rpl_sps_flag[i] && rpl->reference_pic_list.has_value()) {
            const auto& rpl_struct = rpl->reference_pic_list.value();

            // Resize vectors for this list
            if(rpl->poc_lsb_lt.size() <= static_cast<size_t>(i)) {
                rpl->poc_lsb_lt.resize(i + 1);
            }
            if(rpl->delta_poc_msb_cycle_present_flag.size() <= static_cast<size_t>(i)) {
                rpl->delta_poc_msb_cycle_present_flag.resize(i + 1);
            }
            if(rpl->delta_poc_msb_cycle_lt.size() <= static_cast<size_t>(i)) {
                rpl->delta_poc_msb_cycle_lt.resize(i + 1);
            }

            // Get the number of LTRP entries for this list
            int numLtrpEntries = 0;
            if(static_cast<size_t>(i) < rpl->NumLtrpEntries.size() &&
               static_cast<size_t>(rpl->RplsIdx[i]) < rpl->NumLtrpEntries[i].size()) {
                numLtrpEntries = rpl->NumLtrpEntries[i][rpl->RplsIdx[i]];
            }

            for(int j = 0; j < numLtrpEntries; j++) {
                // Check if we need to parse LTRP in header
                bool parseLtrp = false;
                if(sps.sps_long_term_ref_pics_flag &&
                   static_cast<size_t>(i) < rpl_struct.ltrp_in_header_flag.size() &&
                   static_cast<size_t>(rpl->RplsIdx[i]) < rpl_struct.ltrp_in_header_flag[i].size()) {
                    parseLtrp = rpl_struct.ltrp_in_header_flag[i][rpl->RplsIdx[i]];
                }

                if(parseLtrp) {
                    // Parse POC LSB for long-term reference picture
                    uint32_t tmp_poc_lsb_lt = 0;
                    int len_poc_lsb_lt = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
                    TRUE_OR_RETURN(br->ReadBits(len_poc_lsb_lt, &tmp_poc_lsb_lt));

                    // Store POC LSB
                    if(rpl->poc_lsb_lt[i].size() <= static_cast<size_t>(j)) {
                        rpl->poc_lsb_lt[i].resize(j + 1);
                    }
                    rpl->poc_lsb_lt[i][j].push_back(tmp_poc_lsb_lt);

                    // Parse delta POC MSB cycle present flag
                    bool tmp_delta_poc_msb_cycle_present_flag = false;
                    TRUE_OR_RETURN(br->ReadBool(&tmp_delta_poc_msb_cycle_present_flag));

                    if(rpl->delta_poc_msb_cycle_present_flag[i].size() <= static_cast<size_t>(j)) {
                        rpl->delta_poc_msb_cycle_present_flag[i].resize(j + 1);
                    }
                    //rpl->delta_poc_msb_cycle_present_flag[i][j] = tmp_delta_poc_msb_cycle_present_flag;
                    //check_delta_poc_msb_cycle_present_flag = tmp_delta_poc_msb_cycle_present_flag;
                    rpl->delta_poc_msb_cycle_present_flag[i][j] = tmp_delta_poc_msb_cycle_present_flag;

                    // Parse delta POC MSB cycle if present
                    if(tmp_delta_poc_msb_cycle_present_flag) {
                        int tmp_delta_poc_msb_cycle_lt = 0;
                        TRUE_OR_RETURN(br->ReadUE(&tmp_delta_poc_msb_cycle_lt));

                        if(rpl->delta_poc_msb_cycle_lt[i].size() <= static_cast<size_t>(j)) {
                            rpl->delta_poc_msb_cycle_lt[i].resize(j + 1);
                        }
                        rpl->delta_poc_msb_cycle_lt[i][j] = tmp_delta_poc_msb_cycle_lt;
                    }
                }
            }
        }
    } */
    for (int i = 0; i < 2; i++) {
        if (!rpl->rpl_sps_flag[i] && rpl->reference_pic_list.has_value()) {
            const auto& rpl_struct = rpl->reference_pic_list.value();

            // Resize vectors for this list (2D: [listIdx][entryIdx])
            if (rpl->poc_lsb_lt.size() <= static_cast<size_t>(i)) {
                rpl->poc_lsb_lt.resize(i + 1);
            }
            if (rpl->delta_poc_msb_cycle_present_flag.size() <= static_cast<size_t>(i)) {
                rpl->delta_poc_msb_cycle_present_flag.resize(i + 1);
            }
            if (rpl->delta_poc_msb_cycle_lt.size() <= static_cast<size_t>(i)) {
                rpl->delta_poc_msb_cycle_lt.resize(i + 1);
            }

            // Get the number of LTRP entries for this list
            int numLtrpEntries = 0;
            if (static_cast<size_t>(i) < rpl->NumLtrpEntries.size() &&
                static_cast<size_t>(rpl->RplsIdx[i]) < rpl->NumLtrpEntries[i].size()) {
                numLtrpEntries = rpl->NumLtrpEntries[i][rpl->RplsIdx[i]];
            }

            // Ensure the inner vectors are sized correctly
            rpl->poc_lsb_lt[i].resize(numLtrpEntries);
            rpl->delta_poc_msb_cycle_present_flag[i].resize(numLtrpEntries);
            rpl->delta_poc_msb_cycle_lt[i].resize(numLtrpEntries);

            for (int j = 0; j < numLtrpEntries; j++) {
                // Check if we need to parse LTRP in header
                bool parseLtrp = false;
                if (sps.sps_long_term_ref_pics_flag &&
                    static_cast<size_t>(i) < rpl_struct.ltrp_in_header_flag.size() &&
                    static_cast<size_t>(rpl->RplsIdx[i]) < rpl_struct.ltrp_in_header_flag[i].size()) {
                    parseLtrp = rpl_struct.ltrp_in_header_flag[i][rpl->RplsIdx[i]];
                }

                if (parseLtrp) {
                    // Parse POC LSB for long-term reference picture
                    uint32_t tmp_poc_lsb_lt = 0;
                    int len_poc_lsb_lt = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
                    TRUE_OR_RETURN(br->ReadBits(len_poc_lsb_lt, &tmp_poc_lsb_lt));

                    // Store POC LSB (scalaire)
                    if (rpl->poc_lsb_lt[i][j].empty()) {
                       rpl->poc_lsb_lt[i][j].resize(1);
                    }
                    //rpl->poc_lsb_lt[i][j] = tmp_poc_lsb_lt;
                    rpl->poc_lsb_lt[i][j].clear();
                    rpl->poc_lsb_lt[i][j].push_back(static_cast<int>(tmp_poc_lsb_lt));

                    // Parse delta POC MSB cycle present flag
                    bool tmp_delta_poc_msb_cycle_present_flag = false;
                    TRUE_OR_RETURN(br->ReadBool(&tmp_delta_poc_msb_cycle_present_flag));
                    rpl->delta_poc_msb_cycle_present_flag[i][j] = tmp_delta_poc_msb_cycle_present_flag;

                    // Parse delta POC MSB cycle if present
                    if (tmp_delta_poc_msb_cycle_present_flag) {
                        int tmp_delta_poc_msb_cycle_lt = 0;
                        TRUE_OR_RETURN(br->ReadUE(&tmp_delta_poc_msb_cycle_lt));
                        rpl->delta_poc_msb_cycle_lt[i][j] = tmp_delta_poc_msb_cycle_lt;
                    } else {
                        // Initialiser à 0 si pas présent
                        rpl->delta_poc_msb_cycle_lt[i][j] = 0;
                    }
                }
            }
        }
    }

    if(rpl->reference_pic_list.has_value()) {
        rpl->reference_pic_list->is_valid = true;
    }




    return kOk;
}
#if 0
//dissaemble after core core dump
#if 1
H266Parser::Result H266Parser::Ref_Pic_List_Struct(int listIdx, int rplsIdx,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266ReferencePicListStruct* rpls) {
    LOG(INFO) << "Parsing H.266 Reference picture list structure parameters";


/*

    if (!HasVps(sps->sps_video_parameter_set_id)) {
        DLOG(ERROR) << "VPS " << sps->sps_video_parameter_set_id << " not found";
        DebugPrintAvailableSets();
        return kInvalidStream;
     }
    H266Vps* vps = GetVps(sps->sps_video_parameter_set_id);
    if (!vps) {
       DLOG(ERROR) << "GetSps returned nullptr";
      return kInvalidStream;
    }
    if (!vps) {
       DLOG(WARNING) << "Trying first available VPS";
      vps = GetFirstVps();
      if (!vps) {
       DLOG(ERROR) << "No VPS available at all";
       return kInvalidStream;
     }
   }
 */

    //H266Vps *vps = GetVps(sps->sps_video_parameter_set_id);
    //TRUE_OR_RETURN(vps);





    //for this moment i don t have stram with vps...

    //7.4.3.3 (eq 29  PAGE 97)
    // I DON T SAU HOW GET nuh_layer_id FROM NAL HERE FOR THIS MOMENT
    // for (int i = 0; i<= vps->vps_max_layers_minus1; i++){
    //   if(nuh_layer_id == vps->vps_layer_id[i]) {
    //     general_layer_idx = i;
    //     break;
    //   }
    // }
    // if (general_layer_idx < 0) {
    //   DLOG(INFO) << "vps_layer_id " << nuh_layer_id << " not available.\n";
    // }

    // //7.4.3.3 (28)
    // // for (int j = 0; j <= vps->vps_max_layers_minus1; j++) {
    //     if (vps->vps_direct_ref_layer_flag[general_layer_idx][j])
    //         num_direct_ref_layers++;
    // }



    LOG(INFO) << "## listIdx = " << listIdx << "rplsIdx = " << rplsIdx;

    int tmp_num_ref_entries = 0;
    TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
    DLOG(INFO) << "## num_ref_entries " << tmp_num_ref_entries;

    // Initialize num_ref_entries as 2D vector
    if(rpls->num_ref_entries.size() <= static_cast<size_t>(listIdx)) {
        rpls->num_ref_entries.resize(listIdx + 1);
    }
    if(rpls->num_ref_entries[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
        rpls->num_ref_entries[listIdx].resize(rplsIdx + 1);
    }
    rpls->num_ref_entries[listIdx][rplsIdx] = tmp_num_ref_entries;

    // Initialize ltrp_in_header_flag
    bool ltrp_in_header = false;
    if(sps.sps_long_term_ref_pics_flag && rplsIdx < sps.sps_num_ref_pic_lists[listIdx] &&
       tmp_num_ref_entries > 0) {
        TRUE_OR_RETURN(br->ReadBool(&ltrp_in_header));
        DLOG(INFO) << "##  ltrp_in_header : " << ( ltrp_in_header ? "1" : "0");
    }

    if(rpls->ltrp_in_header_flag.size() <= static_cast<size_t>(listIdx)) {
            rpls->ltrp_in_header_flag.resize(listIdx + 1);
    }
    if(rpls->ltrp_in_header_flag[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
            rpls->ltrp_in_header_flag[listIdx].resize(rplsIdx + 1);
    }
    rpls->ltrp_in_header_flag[listIdx][rplsIdx] = ltrp_in_header;


    rpls->entries.clear();

    for(int i = 0; i < tmp_num_ref_entries; i++) {
        H266RefPicListEntry entry;
        entry.inter_layer_ref_pic_flag = false;
        entry.st_ref_pic_flag = true;
        entry.abs_delta_poc_st = 0;
        entry.strp_entry_sign_flag = false;

        if(sps.sps_inter_layer_prediction_enabled_flag) {
            bool tmp_inter_layer_ref_pic_flag = false;
            TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
            entry.inter_layer_ref_pic_flag = tmp_inter_layer_ref_pic_flag;
            DLOG(INFO) << "## inter_layer_ref_pic_flag  : " << ( tmp_inter_layer_ref_pic_flag ? "1" : "0");

        }

        if(!entry.inter_layer_ref_pic_flag) {
            if(sps.sps_long_term_ref_pics_flag) {
                bool tmp_st_ref_pic_flag = false;
                TRUE_OR_RETURN(br->ReadBool(&tmp_st_ref_pic_flag));
                entry.st_ref_pic_flag = tmp_st_ref_pic_flag;
                DLOG(INFO) << "## st_ref_pic_flag : " << ( tmp_st_ref_pic_flag ? "1" : "0");

            }

            if(entry.st_ref_pic_flag) {
                int tmp_abs_delta_poc_st = 0;
                TRUE_OR_RETURN(br->ReadUE(&tmp_abs_delta_poc_st));
                entry.abs_delta_poc_st = tmp_abs_delta_poc_st;
                DLOG(INFO) << "## abs_delta_poc_st : " << tmp_abs_delta_poc_st;


                if(entry.abs_delta_poc_st > 0) {
                    bool tmp_strp_entry_sign_flag = false;
                    TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
                    entry.strp_entry_sign_flag = tmp_strp_entry_sign_flag;
                    DLOG(INFO) << "## strp_entry_sign_flag : " << ( tmp_strp_entry_sign_flag ? "1" : "0");

                }
            } else if(!ltrp_in_header) { // go 4816 line
                uint32_t tmp_rpls_poc_lsb_lt = 0;
                int bit_length = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
                TRUE_OR_RETURN(br->ReadBits(bit_length, &tmp_rpls_poc_lsb_lt));
                entry.rpls_poc_lsb_lt = tmp_rpls_poc_lsb_lt;
                DLOG(INFO) << "## rpls_poc_lsb_lt : " << tmp_rpls_poc_lsb_lt;

            }
        } else {
            int tmp_ilrp_idx = 0;
            TRUE_OR_RETURN(br->ReadUE(&tmp_ilrp_idx));
            entry.ilrp_idx = tmp_ilrp_idx;
            DLOG(INFO) << "## ilrp_idx : " << tmp_ilrp_idx;

        }

        rpls->entries.push_back(entry);
    }

    return kOk;
}

#else

#if 0
H266Parser::Result H266Parser::Ref_Pic_List_Struct(int listIdx, int rplsIdx,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266ReferencePicListStruct* rpls) {
    LOG(INFO) << "Parsing H.266 Reference picture list structure parameters";
    //in use   crash
    int tmp_num_ref_entries = 0;

    TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
    rpls->num_ref_entries[listIdx][rplsIdx] = tmp_num_ref_entries;

    // Initialize ltrp_in_header_flag as vector
    rpls->ltrp_in_header_flag.clear();
    if(sps.sps_long_term_ref_pics_flag && rplsIdx < sps.sps_num_ref_pic_lists[listIdx] &&
       rpls->num_ref_entries > 0) {
        bool tmp_ltrp_in_header_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_ltrp_in_header_flag));
        // Store as single value or resize vector based on your needs
        rpls->ltrp_in_header_flag.resize(1);
        rpls->ltrp_in_header_flag = tmp_ltrp_in_header_flag;
    }

    rpls->entries.clear();
    for(int i = 0; i < rpls->num_ref_entries.size(); i++) {
        H266RefPicListEntry entry;

        if(sps.sps_inter_layer_prediction_enabled_flag) {
            bool tmp_inter_layer_ref_pic_flag = false;
            TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
            entry.inter_layer_ref_pic_flag = tmp_inter_layer_ref_pic_flag;
        }else {
            entry.inter_layer_ref_pic_flag = false;
        }

        if(!entry.inter_layer_ref_pic_flag) {
            if(sps.sps_long_term_ref_pics_flag) {
                bool tmp_st_ref_pic_flag = false;
                TRUE_OR_RETURN(br->ReadBool(&tmp_st_ref_pic_flag));
                entry.st_ref_pic_flag = tmp_st_ref_pic_flag;
            } else {
              entry.st_ref_pic_flag = false;
            }


            if(entry.st_ref_pic_flag) {
                int tmp_abs_delta_poc_st = 0;
                TRUE_OR_RETURN(br->ReadUE(&tmp_abs_delta_poc_st));
                entry.abs_delta_poc_st = tmp_abs_delta_poc_st;
                // eq 150 page 163
                if ((sps->sps_weighted_pred_flag ||
                     sps->sps_weighted_bipred_flag) && i != 0){
                    entry.abs_delta_poc_st = entry.abs_delta_poc_st;
                }
                else
                {
                    entry.abs_delta_poc_st = entry.abs_delta_poc_st + 1;
                }


                if(entry.abs_delta_poc_st > 0) {
                    bool tmp_strp_entry_sign_flag = false;
                    TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
                    entry.strp_entry_sign_flag = tmp_strp_entry_sign_flag;
                }


            } else if(rpls->ltrp_in_header_flag.size() > 0 && !rpls->ltrp_in_header_flag[0]) {
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
#else
// test

H266Parser::Result H266Parser::Ref_Pic_List_Struct(int listIdx, int rplsIdx,
                            const H266Sps& sps,
                            H26xBitReader* br,
                            H266ReferencePicListStruct* rpls) {
    LOG(INFO) << "Parsing H.266 Reference picture list structure parameters";
    LOG(INFO) << "## listIdx = " << listIdx << " rplsIdx = " << rplsIdx;

    int tmp_num_ref_entries = 0;
    TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
    DLOG(INFO) << "## num_ref_entries " << tmp_num_ref_entries;

    // Initialize num_ref_entries as 2D vector
    if (rpls->num_ref_entries.size() <= static_cast<size_t>(listIdx)) {
        rpls->num_ref_entries.resize(listIdx + 1);
    }
    if (rpls->num_ref_entries[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
        rpls->num_ref_entries[listIdx].resize(rplsIdx + 1);
    }
    rpls->num_ref_entries[listIdx][rplsIdx] = tmp_num_ref_entries;

    // Initialize ltrp_in_header_flag
    bool ltrp_in_header = false;
    if (sps.sps_long_term_ref_pics_flag &&
        listIdx < static_cast<int>(sps.sps_num_ref_pic_lists.size()) &&
        rplsIdx < sps.sps_num_ref_pic_lists[listIdx] &&
        tmp_num_ref_entries > 0) {
        TRUE_OR_RETURN(br->ReadBool(&ltrp_in_header));
        DLOG(INFO) << "## ltrp_in_header : " << (ltrp_in_header ? "1" : "0");

        if (rpls->ltrp_in_header_flag.size() <= static_cast<size_t>(listIdx)) {
            rpls->ltrp_in_header_flag.resize(listIdx + 1);
        }
        if (rpls->ltrp_in_header_flag[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
            rpls->ltrp_in_header_flag[listIdx].resize(rplsIdx + 1);
        }
        rpls->ltrp_in_header_flag[listIdx][rplsIdx] = ltrp_in_header;
    }

    // Clear entries for this specific listIdx and rplsIdx
    // We need a 3D structure: listIdx -> rplsIdx -> vector of entries
    if (rpls->entries.size() <= static_cast<size_t>(listIdx)) {
        rpls->entries.resize(listIdx + 1);
    }
    if (rpls->entries[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
        rpls->entries[listIdx].resize(rplsIdx + 1);
    }
    rpls->entries[listIdx][rplsIdx].clear();

    for (int i = 0; i < tmp_num_ref_entries; i++) {
        H266RefPicListEntry entry;

        if (sps.sps_inter_layer_prediction_enabled_flag) {
            bool tmp_inter_layer_ref_pic_flag = false;
            TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
            entry.inter_layer_ref_pic_flag = tmp_inter_layer_ref_pic_flag;
            DLOG(INFO) << "## inter_layer_ref_pic_flag : " <<
                       (tmp_inter_layer_ref_pic_flag ? "1" : "0");
        }

        if (!entry.inter_layer_ref_pic_flag) {
            if (sps.sps_long_term_ref_pics_flag) {
                bool tmp_st_ref_pic_flag = false;
                TRUE_OR_RETURN(br->ReadBool(&tmp_st_ref_pic_flag));
                entry.st_ref_pic_flag = tmp_st_ref_pic_flag;
                DLOG(INFO) << "## st_ref_pic_flag : " <<
                           (tmp_st_ref_pic_flag ? "1" : "0");
            }

            if (entry.st_ref_pic_flag) {
                int tmp_abs_delta_poc_st = 0;
                TRUE_OR_RETURN(br->ReadUE(&tmp_abs_delta_poc_st));
                entry.abs_delta_poc_st = tmp_abs_delta_poc_st;
                DLOG(INFO) << "## abs_delta_poc_st : " << tmp_abs_delta_poc_st;

                if (tmp_abs_delta_poc_st > 0) {
                    bool tmp_strp_entry_sign_flag = false;
                    TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
                    entry.strp_entry_sign_flag = tmp_strp_entry_sign_flag;
                    DLOG(INFO) << "## strp_entry_sign_flag : " <<
                               (tmp_strp_entry_sign_flag ? "1" : "0");
                }
            } else if (!ltrp_in_header) {
                uint32_t tmp_rpls_poc_lsb_lt = 0;
                int bit_length = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
                TRUE_OR_RETURN(br->ReadBits(bit_length, &tmp_rpls_poc_lsb_lt));
                entry.rpls_poc_lsb_lt = tmp_rpls_poc_lsb_lt;
                DLOG(INFO) << "## rpls_poc_lsb_lt : " << tmp_rpls_poc_lsb_lt;
            }
        } else {
            int tmp_ilrp_idx = 0;
            TRUE_OR_RETURN(br->ReadUE(&tmp_ilrp_idx));
            entry.ilrp_idx = tmp_ilrp_idx;
            DLOG(INFO) << "## ilrp_idx : " << tmp_ilrp_idx;
        }

        rpls->entries[listIdx][rplsIdx].push_back(entry);
    }

    return kOk;
}





#endif





#endif
#endif

H266Parser::Result H266Parser::Ref_Pic_List_Struct(
    int listIdx, int rplsIdx,
    const H266Sps& sps,
    H26xBitReader* br,
    H266ReferencePicListStruct* rpls) {

  LOG(INFO) << "Parsing H.266 Reference picture list structure parameters";

  // Validate indices
  if (listIdx < 0 || listIdx >= 2) {  // H.266 has 2 reference lists (L0, L1)
    DLOG(ERROR) << "Invalid listIdx: " << listIdx;
    return kInvalidStream;
  }

  // Read num_ref_entries
  int tmp_num_ref_entries = 0;
  TRUE_OR_RETURN(br->ReadUE(&tmp_num_ref_entries));
  DLOG(INFO) << "## num_ref_entries : " << tmp_num_ref_entries;



  // Resize num_ref_entries to accommodate [listIdx][rplsIdx]
  if (rpls->num_ref_entries.size() <= static_cast<size_t>(listIdx)) {
    rpls->num_ref_entries.resize(listIdx + 1);
  }
  if (rpls->num_ref_entries[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
    rpls->num_ref_entries[listIdx].resize(rplsIdx + 1);
  }
  rpls->num_ref_entries[listIdx][rplsIdx] = tmp_num_ref_entries;

  // Read ltrp_in_header_flag if applicable
  bool ltrp_in_header = false;
  if (sps.sps_long_term_ref_pics_flag &&
      rplsIdx < sps.sps_num_ref_pic_lists[listIdx] &&
      tmp_num_ref_entries > 0) {

    TRUE_OR_RETURN(br->ReadBool(&ltrp_in_header));
    DLOG(INFO) << "## ltrp_in_header : " << ( ltrp_in_header ? "1" : "0");


    // Resize ltrp_in_header_flag
    if (rpls->ltrp_in_header_flag.size() <= static_cast<size_t>(listIdx)) {
      rpls->ltrp_in_header_flag.resize(listIdx + 1);
    }
    if (rpls->ltrp_in_header_flag[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
      rpls->ltrp_in_header_flag[listIdx].resize(rplsIdx + 1);
    }
    rpls->ltrp_in_header_flag[listIdx][rplsIdx] = ltrp_in_header;
  }

  // Resize entries to accommodate [listIdx][rplsIdx]
  // NE PAS utiliser clear() car cela efface tout !
  if (rpls->entries.size() <= static_cast<size_t>(listIdx)) {
    rpls->entries.resize(listIdx + 1);
  }
  if (rpls->entries[listIdx].size() <= static_cast<size_t>(rplsIdx)) {
    rpls->entries[listIdx].resize(rplsIdx + 1);
  }

  // Clear only the current list's entries
  rpls->entries[listIdx][rplsIdx].clear();
  rpls->entries[listIdx][rplsIdx].reserve(tmp_num_ref_entries);

  // Parse each reference entry
  for (int i = 0; i < tmp_num_ref_entries; i++) {
    H266RefPicListEntry entry = {};  // Initialiser à zéro

    // Parse inter_layer_ref_pic_flag
    if (sps.sps_inter_layer_prediction_enabled_flag) {
      bool tmp_inter_layer_ref_pic_flag = false;
      TRUE_OR_RETURN(br->ReadBool(&tmp_inter_layer_ref_pic_flag));
      entry.inter_layer_ref_pic_flag = tmp_inter_layer_ref_pic_flag;
      DLOG(INFO) << "## inter_layer_ref_pic_flag : " << ( tmp_inter_layer_ref_pic_flag ? "1" : "0");

    }

    if (!entry.inter_layer_ref_pic_flag) {
      // Default: short-term reference
      entry.st_ref_pic_flag = true;

      // Parse st_ref_pic_flag if long-term refs are enabled
      if (sps.sps_long_term_ref_pics_flag) {
        bool tmp_st_ref_pic_flag = false;
        TRUE_OR_RETURN(br->ReadBool(&tmp_st_ref_pic_flag));
        entry.st_ref_pic_flag = tmp_st_ref_pic_flag;
        DLOG(INFO) << "## st_ref_pic_flag : " << ( tmp_st_ref_pic_flag ? "1" : "0");

      }

      if (entry.st_ref_pic_flag) {
        // Short-term reference picture
        int tmp_abs_delta_poc_st = 0;
        TRUE_OR_RETURN(br->ReadUE(&tmp_abs_delta_poc_st));
        entry.abs_delta_poc_st = tmp_abs_delta_poc_st;
        DLOG(INFO) << "## abs_delta_poc_st : " << tmp_abs_delta_poc_st;


        if (entry.abs_delta_poc_st > 0) {
          bool tmp_strp_entry_sign_flag = false;
          TRUE_OR_RETURN(br->ReadBool(&tmp_strp_entry_sign_flag));
          entry.strp_entry_sign_flag = tmp_strp_entry_sign_flag;
          DLOG(INFO) << "## strp_entry_sign_flag : " << ( tmp_strp_entry_sign_flag ? "1" : "0");

        }
      } else if (!ltrp_in_header) {
        // Long-term reference picture with POC in structure
        uint32_t tmp_rpls_poc_lsb_lt = 0;
        int bit_length = sps.sps_log2_max_pic_order_cnt_lsb_minus4 + 4;
        TRUE_OR_RETURN(br->ReadBits(bit_length, &tmp_rpls_poc_lsb_lt));
        entry.rpls_poc_lsb_lt = tmp_rpls_poc_lsb_lt;
        DLOG(INFO) << "## rpls_poc_lsb_lt : " << tmp_rpls_poc_lsb_lt;

      }
    } else {
      // Inter-layer reference picture
      int tmp_ilrp_idx = 0;
      TRUE_OR_RETURN(br->ReadUE(&tmp_ilrp_idx));
      entry.ilrp_idx = tmp_ilrp_idx;
      DLOG(INFO) << "## tmp_ilrp_idx : " << tmp_ilrp_idx;

    }

    rpls->entries[listIdx][rplsIdx].push_back(entry);
  }

  DLOG(INFO) << "Parsed " << tmp_num_ref_entries << " ref entries for list L"
             << listIdx << ", index " << rplsIdx;

  return kOk;
}




H266Parser::Result H266Parser::SpsRangeExtension(H26xBitReader* br, bool extended_precision_flag,
                     H266SpsRangeExtension *sps_sre){
  //7.3.2.22 Sequence parameter set range extension syntax
  LOG(INFO) << "Parsing H.266 SpsRangeExtension";
  TRUE_OR_RETURN(br->ReadBool(&sps_sre->sps_extended_precision_flag));
  if(extended_precision_flag){
      TRUE_OR_RETURN(br->ReadBool(&sps_sre->sps_ts_residual_coding_rice_present_in_sh_flag));
  }
  TRUE_OR_RETURN(br->ReadBool(&sps_sre->sps_persistent_rice_adaptation_enabled_flag));
  TRUE_OR_RETURN(br->ReadBool(&sps_sre->sps_reverse_last_sig_coeff_enabled_flag));

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
    TRUE_OR_RETURN(br->ReadUE(&tmp_dpb_max_dec_pic_buffering_minus1));
    DLOG(INFO) << "## dpb_max_dec_pic_buffering_minus1: " << tmp_dpb_max_dec_pic_buffering_minus1 ;

    TRUE_OR_RETURN(br->ReadUE(&tmp_dpb_max_num_reorder_pics));
    DLOG(INFO) << "## dpb_max_num_reorder_pics : " << tmp_dpb_max_num_reorder_pics;
    TRUE_OR_RETURN(br->ReadUE(&tmp_dpb_max_latency_increase_plus1));
    DLOG(INFO) << "## dpb_max_latency_increase_plus1 : " << tmp_dpb_max_latency_increase_plus1;

    dpd->dpb_max_dec_pic_buffering_minus1.push_back(tmp_dpb_max_dec_pic_buffering_minus1);
    dpd->dpb_max_num_reorder_pics.push_back(tmp_dpb_max_num_reorder_pics);
    dpd->dpb_max_latency_increase_plus1.push_back(tmp_dpb_max_latency_increase_plus1);
  }
  return kOk;
}
#if 0
H266Parser::Result H266Parser::Ols_Timing_Hrd_parameters(
    int firstsublayer, int sps_max_sublayers_minus1,
    const H266Sps& sps,
    H26xBitReader* br,
    H266OlsTimingHrdParameters* olf) {

  LOG(INFO) << "Parsing H.266 Ols Timing Hrd parameters";
  const GeneralTimingHrdParameters* timing_hrd = nullptr;
  //const auto* timing_hrd = nullptr; a tester


  //  check it
  if (sps.general_timing_hrd_parameters.has_value()) {
    DLOG(ERROR) << "general_timing_hrd_parameters present in SPS";
    //return kInvalidStream;
  } else {
    DLOG(ERROR) << "general_timing_hrd_parameters not present in SPS";
  }

  if (sps.general_timing_hrd_parameters.has_value()) {
    // get once ref
    timing_hrd = &sps.general_timing_hrd_parameters.value();
  }

  bool tmp_fixed_pic_rate_general_flag = false;
  bool tmp_fixed_pic_rate_within_cvs_flag = false;
  int tmp_elemental_duration_in_tc_minus1 = 0;
  bool tmp_low_delay_hrd_flag = false;

  for (int i = firstsublayer; i <= sps_max_sublayers_minus1; i++) {
    TRUE_OR_RETURN(br->ReadBool(&tmp_fixed_pic_rate_general_flag));
    olf->fixed_pic_rate_general_flag.push_back(tmp_fixed_pic_rate_general_flag);
    DLOG(INFO) << "## fixed_pic_rate_general_flag : "
               << (tmp_fixed_pic_rate_general_flag ? "1" : "0");

    if (!tmp_fixed_pic_rate_general_flag) {
      TRUE_OR_RETURN(br->ReadBool(&tmp_fixed_pic_rate_within_cvs_flag));
      olf->fixed_pic_rate_within_cvs_flag.push_back(tmp_fixed_pic_rate_within_cvs_flag);

      if (tmp_fixed_pic_rate_within_cvs_flag) {
        TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
        olf->elemental_duration_in_tc_minus1.push_back(tmp_elemental_duration_in_tc_minus1);
         DLOG(INFO) << "## elemental_duration_in_tc_minus1 : "  << tmp_elemental_duration_in_tc_minus1;

      } else if (( timing_hrd && timing_hrd->general_nal_hrd_params_present_flag ||
                  timing_hrd->general_vcl_hrd_params_present_flag) &&
                 timing_hrd>hrd_cpb_cnt_minus1 == 0) {

        TRUE_OR_RETURN(br->ReadBool(&tmp_low_delay_hrd_flag));
        olf->low_delay_hrd_flag.push_back(tmp_low_delay_hrd_flag);

        int tmp_cpb_size_value_minus1 = 0;
        int tmp_cpb_size_du_value_minus1 = 0;
        int tmp_bit_rate_du_value_minus1 = 0;
        int tmp_cbr_flag = 0;


        // NAL HRD parameters
        if (timing_hrd->general_nal_hrd_params_present_flag) {
          for (int j = 0; j <= timing_hrd->hrd_cpb_cnt_minus1; j++) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->cpb_size_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);

            if (timing_hrd->general_du_hrd_params_present_flag) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_du_value_minus1));
              olf->cpb_size_du_value_minus1[i][j].push_back(tmp_cpb_size_du_value_minus1);

              TRUE_OR_RETURN(br->ReadUE(&tmp_bit_rate_du_value_minus1));
              olf->bit_rate_du_value_minus1[i][j].push_back(tmp_bit_rate_du_value_minus1);
            }

            bool cbr_flag;
            TRUE_OR_RETURN(br->ReadBool(&cbr_flag));
            tmp_cbr_flag = cbr_flag ? 1 : 0;
            olf->cbr_flag[i][j].push_back(tmp_cbr_flag);
          }
        }

        // VCL HRD parameters
        if (timing_hrd->general_vcl_hrd_params_present_flag) {
          for (int j = 0; j <= timing_hrd->hrd_cpb_cnt_minus1; j++) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->cpb_size_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);

            if (timing_hrd->general_du_hrd_params_present_flag) {
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
  }

  return kOk;
}
#else
H266Parser::Result H266Parser::Ols_Timing_Hrd_parameters(
    int firstsublayer, int sps_max_sublayers_minus1,
    const H266Sps& sps,
    H26xBitReader* br,
    H266OlsTimingHrdParameters* olf) {

  LOG(INFO) << "Parsing H.266 Ols Timing Hrd parameters";

  const GeneralTimingHrdParameters* timing_hrd = nullptr;

  if (sps.general_timing_hrd_parameters.has_value()) {
    timing_hrd = &sps.general_timing_hrd_parameters.value();
  } else {
    DLOG(ERROR) << "general_timing_hrd_parameters not present in SPS";
  }

  bool tmp_fixed_pic_rate_general_flag = false;
  bool tmp_fixed_pic_rate_within_cvs_flag = false;
  int tmp_elemental_duration_in_tc_minus1 = 0;
  bool tmp_low_delay_hrd_flag = false;

  for (int i = firstsublayer; i <= sps_max_sublayers_minus1; i++) {
    TRUE_OR_RETURN(br->ReadBool(&tmp_fixed_pic_rate_general_flag));
    olf->fixed_pic_rate_general_flag.push_back(tmp_fixed_pic_rate_general_flag);
    DLOG(INFO) << "## fixed_pic_rate_general_flag : "
               << (tmp_fixed_pic_rate_general_flag ? "1" : "0");

    if (!tmp_fixed_pic_rate_general_flag) {
      TRUE_OR_RETURN(br->ReadBool(&tmp_fixed_pic_rate_within_cvs_flag));
      olf->fixed_pic_rate_within_cvs_flag.push_back(tmp_fixed_pic_rate_within_cvs_flag);

      if (tmp_fixed_pic_rate_within_cvs_flag) {
        TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
        olf->elemental_duration_in_tc_minus1.push_back(tmp_elemental_duration_in_tc_minus1);
        DLOG(INFO) << "## elemental_duration_in_tc_minus1 : "  << tmp_elemental_duration_in_tc_minus1;

      } else if (timing_hrd &&
                 (timing_hrd->general_nal_hrd_params_present_flag ||
                  timing_hrd->general_vcl_hrd_params_present_flag) &&
                 timing_hrd->hrd_cpb_cnt_minus1 == 0) {

        TRUE_OR_RETURN(br->ReadBool(&tmp_low_delay_hrd_flag));
        olf->low_delay_hrd_flag.push_back(tmp_low_delay_hrd_flag);

        int tmp_cpb_size_value_minus1 = 0;
        int tmp_cpb_size_du_value_minus1 = 0;
        int tmp_bit_rate_du_value_minus1 = 0;
        int tmp_cbr_flag = 0;

        // NAL HRD parameters
        if (timing_hrd->general_nal_hrd_params_present_flag) {
          for (int j = 0; j <= timing_hrd->hrd_cpb_cnt_minus1; j++) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->cpb_size_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);

            if (timing_hrd->general_du_hrd_params_present_flag) {
              TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_du_value_minus1));
              olf->cpb_size_du_value_minus1[i][j].push_back(tmp_cpb_size_du_value_minus1);

              TRUE_OR_RETURN(br->ReadUE(&tmp_bit_rate_du_value_minus1));
              olf->bit_rate_du_value_minus1[i][j].push_back(tmp_bit_rate_du_value_minus1);
            }

            bool cbr_flag;
            TRUE_OR_RETURN(br->ReadBool(&cbr_flag));
            tmp_cbr_flag = cbr_flag ? 1 : 0;
            olf->cbr_flag[i][j].push_back(tmp_cbr_flag);
          }
        }

        // VCL HRD parameters
        if (timing_hrd->general_vcl_hrd_params_present_flag) {
          for (int j = 0; j <= timing_hrd->hrd_cpb_cnt_minus1; j++) {
            TRUE_OR_RETURN(br->ReadUE(&tmp_elemental_duration_in_tc_minus1));
            olf->bit_rate_value_minus1[i][j].push_back(tmp_elemental_duration_in_tc_minus1);

            TRUE_OR_RETURN(br->ReadUE(&tmp_cpb_size_value_minus1));
            olf->cpb_size_value_minus1[i][j].push_back(tmp_cpb_size_value_minus1);

            if (timing_hrd->general_du_hrd_params_present_flag) {
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
  }

  return kOk;
}

#endif




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



void H266Parser::DebugPrintAvailableSets() const {
  DLOG(INFO) << "=== Available Parameter Sets ===";

  DLOG(INFO) << "VPS (" << active_vpses_.size() << "):";
  for (const auto& pair : active_vpses_) {
    DLOG(INFO) << "  - VPS id " << pair.first
               << " valid=" << (pair.second != nullptr);
  }

  DLOG(INFO) << "SPS (" << active_spses_.size() << "):";
  for (const auto& pair : active_spses_) {
    DLOG(INFO) << "  - SPS id " << pair.first
               << " valid=" << (pair.second != nullptr)
               << " vps_id=" << (pair.second ? pair.second->sps_video_parameter_set_id : -1);
  }

  DLOG(INFO) << "PPS (" << active_ppses_.size() << "):";
  for (const auto& pair : active_ppses_) {
    DLOG(INFO) << "  - PPS id " << pair.first
               << " valid=" << (pair.second != nullptr)
               << " sps_id=" << (pair.second ? pair.second->pps_seq_parameter_set_id : -1);
  }

  DLOG(INFO) << "================================";
}


}  // namespace media
}  // namespace shaka

