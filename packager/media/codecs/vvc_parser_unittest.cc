// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/codecs/vvc_parser.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace shaka {
namespace media {

class H266Parser Test : public testing::Test {
 protected:
  H266Parser  parser_;
};

TEST_F(H266Parser Test, ExtractNalUnitHeader) {
  // VPS NAL header: layer_id=0, type=14(VPS), temporal_id=0
  // Format: forbidden(1) reserved(1) layer_id(6) type(5) temporal_id_plus1(3)
  // 0000 0000 0111 0001 = 0x0071
  uint8_t vps_header[] = {0x00, 0x71};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(H266Parser ::ExtractNalUnitHeader(vps_header, 2, 
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(14, type);  // VPS
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(H266Parser Test, ExtractSpsHeader) {
  // SPS NAL header: layer_id=0, type=15(SPS), temporal_id=0
  // 0000 0000 0111 1001 = 0x0079
  uint8_t sps_header[] = {0x00, 0x79};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(H266Parser ::ExtractNalUnitHeader(sps_header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(15, type);  // SPS
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(H266Parser Test, ExtractPpsHeader) {
  // PPS NAL header: layer_id=0, type=16(PPS), temporal_id=0
  // 0000 0000 1000 0001 = 0x0081
  uint8_t pps_header[] = {0x00, 0x81};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(H266Parser ::ExtractNalUnitHeader(pps_header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(16, type);  // PPS
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(H266Parser Test, ExtractIdrHeader) {
  // IDR NAL header: layer_id=0, type=7(IDR_W_RADL), temporal_id=0
  // 0000 0000 0011 1001 = 0x0039
  uint8_t idr_header[] = {0x00, 0x39};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(H266Parser ::ExtractNalUnitHeader(idr_header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(7, type);  // IDR_W_RADL
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(H266Parser Test, ExtractHeaderWithLayerId) {
  // NAL header with layer_id=3: layer_id=3, type=15(SPS), temporal_id=2
  // 0000 0110 0111 1101 = 0x067D
  uint8_t header[] = {0x06, 0x7D};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(H266Parser ::ExtractNalUnitHeader(header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(15, type);  // SPS
  EXPECT_EQ(3, layer_id);
  EXPECT_EQ(2, temporal_id);
}

TEST_F(H266Parser Test, InvalidHeaderForbiddenBit) {
  // Header with forbidden_zero_bit set
  uint8_t invalid_header[] = {0x80, 0x71};
  
  uint8_t type, layer_id, temporal_id;
  EXPECT_FALSE(H266Parser ::ExtractNalUnitHeader(invalid_header, 2,
                                                &type, &layer_id, &temporal_id));
}

TEST_F(H266Parser Test, InvalidHeaderTemporalId) {
  // Header with temporal_id_plus1 = 0 (invalid)
  // 0000 0000 0111 0000 = 0x0070
  uint8_t invalid_header[] = {0x00, 0x70};
  
  uint8_t type, layer_id, temporal_id;
  EXPECT_FALSE(H266Parser ::ExtractNalUnitHeader(invalid_header, 2,
                                                &type, &layer_id, &temporal_id));
}

TEST_F(H266Parser Test, FindStartCodes3Byte) {
  uint8_t data[] = {
    0x00, 0x00, 0x01,  // Start code
    0x12, 0x34,        // Data
    0x00, 0x00, 0x01,  // Start code
    0x56, 0x78         // Data
  };
  
  std::vector<size_t> start_codes;
  EXPECT_EQ(2u, H266Parser ::FindStartCodes(data, sizeof(data), &start_codes));
  EXPECT_EQ(2u, start_codes.size());
  EXPECT_EQ(0u, start_codes[0]);
  EXPECT_EQ(5u, start_codes[1]);
}

TEST_F(H266Parser Test, FindStartCodes4Byte) {
  uint8_t data[] = {
    0x00, 0x00, 0x00, 0x01,  // Start code (4 bytes)
    0x12, 0x34,              // Data
    0x00, 0x00, 0x00, 0x01,  // Start code (4 bytes)
    0x56, 0x78               // Data
  };
  
  std::vector<size_t> start_codes;
  EXPECT_EQ(2u, H266Parser ::FindStartCodes(data, sizeof(data), &start_codes));
  EXPECT_EQ(2u, start_codes.size());
  EXPECT_EQ(0u, start_codes[0]);
  EXPECT_EQ(6u, start_codes[1]);
}

TEST_F(H266Parser Test, FindStartCodesMixed) {
  uint8_t data[] = {
    0x00, 0x00, 0x01,        // 3-byte start code
    0x12,
    0x00, 0x00, 0x00, 0x01,  // 4-byte start code
    0x34
  };
  
  std::vector<size_t> start_codes;
  EXPECT_EQ(2u, H266Parser ::FindStartCodes(data, sizeof(data), &start_codes));
  EXPECT_EQ(2u, start_codes.size());
  EXPECT_EQ(0u, start_codes[0]);
  EXPECT_EQ(4u, start_codes[1]);
}

TEST_F(H266Parser Test, ParseSingleNalUnit) {
  // VPS NAL unit
  uint8_t vps_data[] = {
    0x00, 0x71,  // VPS header
    0x01, 0x02, 0x03  // VPS data
  };
  
  H266Parser ::NalUnit nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(vps_data, sizeof(vps_data), &nalu));
  
  EXPECT_EQ(H266Parser ::kVps, nalu.type);
  EXPECT_EQ(0, nalu.layer_id);
  EXPECT_EQ(0, nalu.temporal_id);
  EXPECT_EQ(sizeof(vps_data), nalu.size);
  EXPECT_TRUE(nalu.IsParameterSet());
  EXPECT_FALSE(nalu.IsVcl());
}

TEST_F(H266Parser Test, ParseMultipleNalUnits) {
  uint8_t data[] = {
    // VPS
    0x00, 0x00, 0x01,
    0x00, 0x71, 0xAA,
    // SPS
    0x00, 0x00, 0x01,
    0x00, 0x79, 0xBB,
    // PPS
    0x00, 0x00, 0x01,
    0x00, 0x81, 0xCC
  };
  
  std::vector<H266Parser ::NalUnit> nal_units;
  ASSERT_TRUE(parser_.ParseNalUnits(data, sizeof(data), &nal_units));
  
  EXPECT_EQ(3u, nal_units.size());
  
  EXPECT_EQ(H266Parser ::kVps, nal_units[0].type);
  EXPECT_EQ(H266Parser ::kSps, nal_units[1].type);
  EXPECT_EQ(H266Parser ::kPps, nal_units[2].type);
  
  EXPECT_TRUE(nal_units[0].IsParameterSet());
  EXPECT_TRUE(nal_units[1].IsParameterSet());
  EXPECT_TRUE(nal_units[2].IsParameterSet());
}

TEST_F(H266Parser Test, ConvertAnnexBToLengthPrefixed) {
  uint8_t annexb_data[] = {
    // NAL 1
    0x00, 0x00, 0x01,
    0x00, 0x71, 0xAA, 0xBB,
    // NAL 2
    0x00, 0x00, 0x01,
    0x00, 0x79, 0xCC
  };
  
  std::vector<uint8_t> output;
  ASSERT_TRUE(H266Parser ::ConvertAnnexBToLengthPrefixed(
      annexb_data, sizeof(annexb_data), 4, &output));
  
  // Expected: length(4) + data(4) + length(4) + data(3)
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x04,  // Length = 4
    0x00, 0x71, 0xAA, 0xBB,  // NAL 1 data
    0x00, 0x00, 0x00, 0x03,  // Length = 3
    0x00, 0x79, 0xCC         // NAL 2 data
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(H266Parser Test, ConvertLengthPrefixedToAnnexB) {
  uint8_t length_prefixed[] = {
    0x00, 0x00, 0x00, 0x04,  // Length = 4
    0x00, 0x71, 0xAA, 0xBB,  // NAL 1 data
    0x00, 0x00, 0x00, 0x03,  // Length = 3
    0x00, 0x79, 0xCC         // NAL 2 data
  };
  
  std::vector<uint8_t> output;
  ASSERT_TRUE(H266Parser ::ConvertLengthPrefixedToAnnexB(
      length_prefixed, sizeof(length_prefixed), 4, &output));
  
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x00, 0x71, 0xAA, 0xBB,  // NAL 1 data
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x00, 0x79, 0xCC         // NAL 2 data
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(H266Parser Test, RemoveEmulationPrevention) {
  uint8_t data[] = {
    0x00, 0x00, 0x03, 0x00,  // 0x000003 00 -> 0x00 00 00
    0x00, 0x00, 0x03, 0x01,  // 0x000003 01 -> 0x00 00 01
    0x00, 0x00, 0x03, 0x02,  // 0x000003 02 -> 0x00 00 02
    0x00, 0x00, 0x03, 0x03   // 0x000003 03 -> 0x00 00 03
  };
  
  std::vector<uint8_t> output;
  H266Parser ::RemoveEmulationPrevention(data, sizeof(data), &output);
  
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x01,
    0x00, 0x00, 0x02,
    0x00, 0x00, 0x03
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(H266Parser Test, AddEmulationPrevention) {
  uint8_t data[] = {
    0x00, 0x00, 0x00,  // Needs emulation prevention
    0x00, 0x00, 0x01,  // Needs emulation prevention
    0x00, 0x00, 0x02,  // Needs emulation prevention
    0x00, 0x00, 0x03   // Needs emulation prevention
  };
  
  std::vector<uint8_t> output;
  H266Parser ::AddEmulationPrevention(data, sizeof(data), &output);
  
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x03, 0x01,
    0x00, 0x00, 0x03, 0x02,
    0x00, 0x00, 0x03, 0x03
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(H266Parser Test, NalUnitIsKeyframe) {
  // IDR_W_RADL
  uint8_t idr_header[] = {0x00, 0x39};
  H266Parser ::NalUnit idr_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(idr_header, 2, &idr_nalu));
  EXPECT_TRUE(idr_nalu.IsKeyframe());
  EXPECT_TRUE(idr_nalu.IsIdr());
  
  // IDR_N_LP
  uint8_t idr_nlp_header[] = {0x00, 0x41};
  H266Parser ::NalUnit idr_nlp_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(idr_nlp_header, 2, &idr_nlp_nalu));
  EXPECT_TRUE(idr_nlp_nalu.IsKeyframe());
  EXPECT_TRUE(idr_nlp_nalu.IsIdr());
  
  // CRA
  uint8_t cra_header[] = {0x00, 0x49};
  H266Parser ::NalUnit cra_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(cra_header, 2, &cra_nalu));
  EXPECT_TRUE(cra_nalu.IsKeyframe());
  EXPECT_FALSE(cra_nalu.IsIdr());
}

TEST_F(H266Parser Test, NalUnitIsVcl) {
  // TRAIL (VCL)
  uint8_t trail_header[] = {0x00, 0x01};
  H266Parser ::NalUnit trail_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(trail_header, 2, &trail_nalu));
  EXPECT_TRUE(trail_nalu.IsVcl());
  EXPECT_FALSE(trail_nalu.IsParameterSet());
  
  // SPS (non-VCL)
  uint8_t sps_header[] = {0x00, 0x79};
  H266Parser ::NalUnit sps_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(sps_header, 2, &sps_nalu));
  EXPECT_FALSE(sps_nalu.IsVcl());
  EXPECT_TRUE(sps_nalu.IsParameterSet());
}

}  // namespace media
}  // namespace shaka