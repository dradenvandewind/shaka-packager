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

class VvcParserTest : public testing::Test {
 protected:
  VvcParser parser_;
};

TEST_F(VvcParserTest, ExtractNalUnitHeader) {
  // VPS NAL header: layer_id=0, type=14(VPS), temporal_id=0
  // Format: forbidden(1) reserved(1) layer_id(6) type(5) temporal_id_plus1(3)
  // 0000 0000 0111 0001 = 0x0071
  uint8_t vps_header[] = {0x00, 0x71};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(VvcParser::ExtractNalUnitHeader(vps_header, 2, 
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(14, type);  // VPS
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(VvcParserTest, ExtractSpsHeader) {
  // SPS NAL header: layer_id=0, type=15(SPS), temporal_id=0
  // 0000 0000 0111 1001 = 0x0079
  uint8_t sps_header[] = {0x00, 0x79};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(VvcParser::ExtractNalUnitHeader(sps_header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(15, type);  // SPS
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(VvcParserTest, ExtractPpsHeader) {
  // PPS NAL header: layer_id=0, type=16(PPS), temporal_id=0
  // 0000 0000 1000 0001 = 0x0081
  uint8_t pps_header[] = {0x00, 0x81};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(VvcParser::ExtractNalUnitHeader(pps_header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(16, type);  // PPS
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(VvcParserTest, ExtractIdrHeader) {
  // IDR NAL header: layer_id=0, type=7(IDR_W_RADL), temporal_id=0
  // 0000 0000 0011 1001 = 0x0039
  uint8_t idr_header[] = {0x00, 0x39};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(VvcParser::ExtractNalUnitHeader(idr_header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(7, type);  // IDR_W_RADL
  EXPECT_EQ(0, layer_id);
  EXPECT_EQ(0, temporal_id);
}

TEST_F(VvcParserTest, ExtractHeaderWithLayerId) {
  // NAL header with layer_id=3: layer_id=3, type=15(SPS), temporal_id=2
  // 0000 0110 0111 1101 = 0x067D
  uint8_t header[] = {0x06, 0x7D};
  
  uint8_t type, layer_id, temporal_id;
  ASSERT_TRUE(VvcParser::ExtractNalUnitHeader(header, 2,
                                               &type, &layer_id, &temporal_id));
  
  EXPECT_EQ(15, type);  // SPS
  EXPECT_EQ(3, layer_id);
  EXPECT_EQ(2, temporal_id);
}

TEST_F(VvcParserTest, InvalidHeaderForbiddenBit) {
  // Header with forbidden_zero_bit set
  uint8_t invalid_header[] = {0x80, 0x71};
  
  uint8_t type, layer_id, temporal_id;
  EXPECT_FALSE(VvcParser::ExtractNalUnitHeader(invalid_header, 2,
                                                &type, &layer_id, &temporal_id));
}

TEST_F(VvcParserTest, InvalidHeaderTemporalId) {
  // Header with temporal_id_plus1 = 0 (invalid)
  // 0000 0000 0111 0000 = 0x0070
  uint8_t invalid_header[] = {0x00, 0x70};
  
  uint8_t type, layer_id, temporal_id;
  EXPECT_FALSE(VvcParser::ExtractNalUnitHeader(invalid_header, 2,
                                                &type, &layer_id, &temporal_id));
}

TEST_F(VvcParserTest, FindStartCodes3Byte) {
  uint8_t data[] = {
    0x00, 0x00, 0x01,  // Start code
    0x12, 0x34,        // Data
    0x00, 0x00, 0x01,  // Start code
    0x56, 0x78         // Data
  };
  
  std::vector<size_t> start_codes;
  EXPECT_EQ(2u, VvcParser::FindStartCodes(data, sizeof(data), &start_codes));
  EXPECT_EQ(2u, start_codes.size());
  EXPECT_EQ(0u, start_codes[0]);
  EXPECT_EQ(5u, start_codes[1]);
}

TEST_F(VvcParserTest, FindStartCodes4Byte) {
  uint8_t data[] = {
    0x00, 0x00, 0x00, 0x01,  // Start code (4 bytes)
    0x12, 0x34,              // Data
    0x00, 0x00, 0x00, 0x01,  // Start code (4 bytes)
    0x56, 0x78               // Data
  };
  
  std::vector<size_t> start_codes;
  EXPECT_EQ(2u, VvcParser::FindStartCodes(data, sizeof(data), &start_codes));
  EXPECT_EQ(2u, start_codes.size());
  EXPECT_EQ(0u, start_codes[0]);
  EXPECT_EQ(6u, start_codes[1]);
}

TEST_F(VvcParserTest, FindStartCodesMixed) {
  uint8_t data[] = {
    0x00, 0x00, 0x01,        // 3-byte start code
    0x12,
    0x00, 0x00, 0x00, 0x01,  // 4-byte start code
    0x34
  };
  
  std::vector<size_t> start_codes;
  EXPECT_EQ(2u, VvcParser::FindStartCodes(data, sizeof(data), &start_codes));
  EXPECT_EQ(2u, start_codes.size());
  EXPECT_EQ(0u, start_codes[0]);
  EXPECT_EQ(4u, start_codes[1]);
}

TEST_F(VvcParserTest, ParseSingleNalUnit) {
  // VPS NAL unit
  uint8_t vps_data[] = {
    0x00, 0x71,  // VPS header
    0x01, 0x02, 0x03  // VPS data
  };
  
  VvcParser::NalUnit nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(vps_data, sizeof(vps_data), &nalu));
  
  EXPECT_EQ(VvcParser::kVps, nalu.type);
  EXPECT_EQ(0, nalu.layer_id);
  EXPECT_EQ(0, nalu.temporal_id);
  EXPECT_EQ(sizeof(vps_data), nalu.size);
  EXPECT_TRUE(nalu.IsParameterSet());
  EXPECT_FALSE(nalu.IsVcl());
}

TEST_F(VvcParserTest, ParseMultipleNalUnits) {
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
  
  std::vector<VvcParser::NalUnit> nal_units;
  ASSERT_TRUE(parser_.ParseNalUnits(data, sizeof(data), &nal_units));
  
  EXPECT_EQ(3u, nal_units.size());
  
  EXPECT_EQ(VvcParser::kVps, nal_units[0].type);
  EXPECT_EQ(VvcParser::kSps, nal_units[1].type);
  EXPECT_EQ(VvcParser::kPps, nal_units[2].type);
  
  EXPECT_TRUE(nal_units[0].IsParameterSet());
  EXPECT_TRUE(nal_units[1].IsParameterSet());
  EXPECT_TRUE(nal_units[2].IsParameterSet());
}

TEST_F(VvcParserTest, ConvertAnnexBToLengthPrefixed) {
  uint8_t annexb_data[] = {
    // NAL 1
    0x00, 0x00, 0x01,
    0x00, 0x71, 0xAA, 0xBB,
    // NAL 2
    0x00, 0x00, 0x01,
    0x00, 0x79, 0xCC
  };
  
  std::vector<uint8_t> output;
  ASSERT_TRUE(VvcParser::ConvertAnnexBToLengthPrefixed(
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

TEST_F(VvcParserTest, ConvertLengthPrefixedToAnnexB) {
  uint8_t length_prefixed[] = {
    0x00, 0x00, 0x00, 0x04,  // Length = 4
    0x00, 0x71, 0xAA, 0xBB,  // NAL 1 data
    0x00, 0x00, 0x00, 0x03,  // Length = 3
    0x00, 0x79, 0xCC         // NAL 2 data
  };
  
  std::vector<uint8_t> output;
  ASSERT_TRUE(VvcParser::ConvertLengthPrefixedToAnnexB(
      length_prefixed, sizeof(length_prefixed), 4, &output));
  
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x00, 0x71, 0xAA, 0xBB,  // NAL 1 data
    0x00, 0x00, 0x00, 0x01,  // Start code
    0x00, 0x79, 0xCC         // NAL 2 data
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(VvcParserTest, RemoveEmulationPrevention) {
  uint8_t data[] = {
    0x00, 0x00, 0x03, 0x00,  // 0x000003 00 -> 0x00 00 00
    0x00, 0x00, 0x03, 0x01,  // 0x000003 01 -> 0x00 00 01
    0x00, 0x00, 0x03, 0x02,  // 0x000003 02 -> 0x00 00 02
    0x00, 0x00, 0x03, 0x03   // 0x000003 03 -> 0x00 00 03
  };
  
  std::vector<uint8_t> output;
  VvcParser::RemoveEmulationPrevention(data, sizeof(data), &output);
  
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x00,
    0x00, 0x00, 0x01,
    0x00, 0x00, 0x02,
    0x00, 0x00, 0x03
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(VvcParserTest, AddEmulationPrevention) {
  uint8_t data[] = {
    0x00, 0x00, 0x00,  // Needs emulation prevention
    0x00, 0x00, 0x01,  // Needs emulation prevention
    0x00, 0x00, 0x02,  // Needs emulation prevention
    0x00, 0x00, 0x03   // Needs emulation prevention
  };
  
  std::vector<uint8_t> output;
  VvcParser::AddEmulationPrevention(data, sizeof(data), &output);
  
  std::vector<uint8_t> expected = {
    0x00, 0x00, 0x03, 0x00,
    0x00, 0x00, 0x03, 0x01,
    0x00, 0x00, 0x03, 0x02,
    0x00, 0x00, 0x03, 0x03
  };
  
  EXPECT_EQ(expected, output);
}

TEST_F(VvcParserTest, NalUnitIsKeyframe) {
  // IDR_W_RADL
  uint8_t idr_header[] = {0x00, 0x39};
  VvcParser::NalUnit idr_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(idr_header, 2, &idr_nalu));
  EXPECT_TRUE(idr_nalu.IsKeyframe());
  EXPECT_TRUE(idr_nalu.IsIdr());
  
  // IDR_N_LP
  uint8_t idr_nlp_header[] = {0x00, 0x41};
  VvcParser::NalUnit idr_nlp_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(idr_nlp_header, 2, &idr_nlp_nalu));
  EXPECT_TRUE(idr_nlp_nalu.IsKeyframe());
  EXPECT_TRUE(idr_nlp_nalu.IsIdr());
  
  // CRA
  uint8_t cra_header[] = {0x00, 0x49};
  VvcParser::NalUnit cra_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(cra_header, 2, &cra_nalu));
  EXPECT_TRUE(cra_nalu.IsKeyframe());
  EXPECT_FALSE(cra_nalu.IsIdr());
}

TEST_F(VvcParserTest, NalUnitIsVcl) {
  // TRAIL (VCL)
  uint8_t trail_header[] = {0x00, 0x01};
  VvcParser::NalUnit trail_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(trail_header, 2, &trail_nalu));
  EXPECT_TRUE(trail_nalu.IsVcl());
  EXPECT_FALSE(trail_nalu.IsParameterSet());
  
  // SPS (non-VCL)
  uint8_t sps_header[] = {0x00, 0x79};
  VvcParser::NalUnit sps_nalu;
  ASSERT_TRUE(parser_.ParseNalUnit(sps_header, 2, &sps_nalu));
  EXPECT_FALSE(sps_nalu.IsVcl());
  EXPECT_TRUE(sps_nalu.IsParameterSet());
}

}  // namespace media
}  // namespace shaka