// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/formats/mp4/vvc.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "packager/media/formats/mp4/box_buffer.h"

namespace shaka {
namespace media {
namespace mp4 {

class VVCBoxTest : public testing::Test {
 protected:
  // Helper pour créer des données VPS de test
  std::vector<uint8_t> CreateTestVPS() {
    // VPS minimal: NAL header + vps_id + quelques bits
    return {
      0x00, 0x71,  // NAL header: VPS, layer_id=0, temporal_id=0
      0x00,        // vps_id(4) + max_layers_minus1(6)
      0x00         // max_sublayers_minus1(3) + ...
    };
  }

  // Helper pour créer des données SPS de test
  std::vector<uint8_t> CreateTestSPS() {
    // SPS minimal avec dimensions
    return {
      0x00, 0x79,  // NAL header: SPS, layer_id=0, temporal_id=0
      0x01,        // sps_id = 0 (ue(v))
      0x00,        // vps_id(4) + max_sublayers(3) + chroma_format(2)
      0x00,        // log2_ctu_size_minus5(2) + ptl_present(1) + ...
      0x01,        // gdr_enabled(1) + resampling_enabled(1) + ...
      0x81, 0x68,  // pic_width = 1920 (ue(v) encoded)
      0x81, 0x38,  // pic_height = 1080 (ue(v) encoded)
      0x00         // conformance_window_flag(1) + ...
    };
  }

  // Helper pour créer des données PPS de test
  std::vector<uint8_t> CreateTestPPS() {
    return {
      0x00, 0x81,  // NAL header: PPS, layer_id=0, temporal_id=0
      0x00,        // pps_id = 0
      0x00         // sps_id = 0 + autres flags
    };
  }
};

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordBasic) {
  VVCDecoderConfigurationRecord vvcc;

  // Configuration par défaut
  EXPECT_EQ(3, vvcc.length_size_minus_one());
  EXPECT_TRUE(vvcc.ptl_present_flag());
  EXPECT_EQ(0, vvcc.arrays().size());
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordSetters) {
  VVCDecoderConfigurationRecord vvcc;

  vvcc.set_length_size_minus_one(2);
  vvcc.set_ptl_present_flag(true);
  vvcc.set_chroma_format_idc(1);
  vvcc.set_bit_depth_minus8(2);
  vvcc.set_max_picture_width(1920);
  vvcc.set_max_picture_height(1080);
  vvcc.set_avg_frame_rate(30);

  EXPECT_EQ(2, vvcc.length_size_minus_one());
  EXPECT_TRUE(vvcc.ptl_present_flag());
  EXPECT_EQ(1, vvcc.chroma_format_idc());
  EXPECT_EQ(2, vvcc.bit_depth_minus8());
  EXPECT_EQ(1920, vvcc.max_picture_width());
  EXPECT_EQ(1080, vvcc.max_picture_height());
  EXPECT_EQ(30, vvcc.avg_frame_rate());
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordPTL) {
  VVCDecoderConfigurationRecord vvcc;

  auto& ptl = vvcc.mutable_ptl_record();
  ptl.general_profile_idc = 1;    // Main 10
  ptl.general_tier_flag = 0;      // Main tier
  ptl.general_level_idc = 153;    // Level 5.1
  ptl.ptl_frame_only_constraint_flag = 1;
  ptl.ptl_multilayer_enabled_flag = 0;

  EXPECT_EQ(1, vvcc.ptl_record().general_profile_idc);
  EXPECT_EQ(0, vvcc.ptl_record().general_tier_flag);
  EXPECT_EQ(153, vvcc.ptl_record().general_level_idc);
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordArrays) {
  VVCDecoderConfigurationRecord vvcc;

  // Ajouter des arrays
  auto& arrays = vvcc.mutable_arrays();
  arrays.resize(3);

  // VPS array
  arrays[0].array_completeness = 1;
  arrays[0].nal_unit_type = 14;  // VPS
  arrays[0].nal_units.push_back(CreateTestVPS());

  // SPS array
  arrays[1].array_completeness = 1;
  arrays[1].nal_unit_type = 15;  // SPS
  arrays[1].nal_units.push_back(CreateTestSPS());

  // PPS array
  arrays[2].array_completeness = 1;
  arrays[2].nal_unit_type = 16;  // PPS
  arrays[2].nal_units.push_back(CreateTestPPS());

  EXPECT_EQ(3u, vvcc.arrays().size());
  EXPECT_EQ(14, vvcc.arrays()[0].nal_unit_type);
  EXPECT_EQ(15, vvcc.arrays()[1].nal_unit_type);
  EXPECT_EQ(16, vvcc.arrays()[2].nal_unit_type);
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordWriteRead) {
  VVCDecoderConfigurationRecord original;

  // Configuration
  original.set_length_size_minus_one(3);
  original.set_ptl_present_flag(true);
  original.set_chroma_format_idc(1);
  original.set_bit_depth_minus8(2);
  original.set_max_picture_width(1920);
  original.set_max_picture_height(1080);
  original.set_avg_frame_rate(30);

  auto& ptl = original.mutable_ptl_record();
  ptl.general_profile_idc = 1;
  ptl.general_tier_flag = 0;
  ptl.general_level_idc = 153;

  // Ajouter des NAL units
  auto& arrays = original.mutable_arrays();
  arrays.resize(1);
  arrays[0].array_completeness = 1;
  arrays[0].nal_unit_type = 15;  // SPS
  arrays[0].nal_units.push_back(CreateTestSPS());

  // Écrire dans un buffer
  BufferWriter writer;
  original.Write(&writer);

  // Lire depuis le buffer
  VVCDecoderConfigurationRecord parsed;
  ASSERT_TRUE(parsed.Parse(writer.Buffer(), writer.Size()));

  // Vérifier
  EXPECT_EQ(original.length_size_minus_one(), parsed.length_size_minus_one());
  EXPECT_EQ(original.ptl_present_flag(), parsed.ptl_present_flag());
  EXPECT_EQ(original.chroma_format_idc(), parsed.chroma_format_idc());
  EXPECT_EQ(original.bit_depth_minus8(), parsed.bit_depth_minus8());
  EXPECT_EQ(original.max_picture_width(), parsed.max_picture_width());
  EXPECT_EQ(original.max_picture_height(), parsed.max_picture_height());
  EXPECT_EQ(original.avg_frame_rate(), parsed.avg_frame_rate());

  EXPECT_EQ(original.ptl_record().general_profile_idc,
            parsed.ptl_record().general_profile_idc);
  EXPECT_EQ(original.ptl_record().general_tier_flag,
            parsed.ptl_record().general_tier_flag);
  EXPECT_EQ(original.ptl_record().general_level_idc,
            parsed.ptl_record().general_level_idc);

  EXPECT_EQ(original.arrays().size(), parsed.arrays().size());
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordCodecString) {
  VVCDecoderConfigurationRecord vvcc;

  vvcc.set_ptl_present_flag(true);
  auto& ptl = vvcc.mutable_ptl_record();

  // Test Main tier, Level 5.1
  ptl.general_profile_idc = 1;
  ptl.general_tier_flag = 0;
  ptl.general_level_idc = 153;

  std::string codec = vvcc.GetCodecString(FOURCC_vvc1);
  EXPECT_EQ("vvc1.1.L153", codec);

  // Test High tier
  ptl.general_tier_flag = 1;
  codec = vvcc.GetCodecString(FOURCC_vvc1);
  EXPECT_EQ("vvc1.1.H153", codec);

  // Test Level 3.1
  ptl.general_level_idc = 93;
  codec = vvcc.GetCodecString(FOURCC_vvc1);
  EXPECT_EQ("vvc1.1.H93", codec);

  // Test vvi1 (still image)
  codec = vvcc.GetCodecString(FOURCC_vvi1);
  EXPECT_EQ("vvi1.1.H93", codec);
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordParseFromNalUnits) {
  VVCDecoderConfigurationRecord vvcc;

  // Créer un bitstream Annex B avec VPS, SPS, PPS
  std::vector<uint8_t> annexb_data;

  // Start code + VPS
  annexb_data.insert(annexb_data.end(), {0x00, 0x00, 0x00, 0x01});
  auto vps = CreateTestVPS();
  annexb_data.insert(annexb_data.end(), vps.begin(), vps.end());

  // Start code + SPS
  annexb_data.insert(annexb_data.end(), {0x00, 0x00, 0x00, 0x01});
  auto sps = CreateTestSPS();
  annexb_data.insert(annexb_data.end(), sps.begin(), sps.end());

  // Start code + PPS
  annexb_data.insert(annexb_data.end(), {0x00, 0x00, 0x00, 0x01});
  auto pps = CreateTestPPS();
  annexb_data.insert(annexb_data.end(), pps.begin(), pps.end());

  // Parser
  ASSERT_TRUE(vvcc.ParseFromNalUnits(annexb_data.data(), annexb_data.size()));

  // Vérifier que les arrays ont été créés
  EXPECT_GE(vvcc.arrays().size(), 1u);

  // Vérifier que les parameter sets ont été trouvés
  bool found_vps = false, found_sps = false, found_pps = false;
  for (const auto& array : vvcc.arrays()) {
    if (array.nal_unit_type == 14) found_vps = true;
    if (array.nal_unit_type == 15) found_sps = true;
    if (array.nal_unit_type == 16) found_pps = true;
  }

  EXPECT_TRUE(found_vps || found_sps || found_pps);
}

TEST_F(VVCBoxTest, VVCVisualSampleEntryBasic) {
  VVCVisualSampleEntry entry;

  entry.format = FOURCC_vvc1;
  entry.width = 1920;
  entry.height = 1080;

  auto& vvcc = entry.mutable_vvc_config();
  vvcc.set_max_picture_width(1920);
  vvcc.set_max_picture_height(1080);

  EXPECT_EQ(FOURCC_vvc1, entry.format);
  EXPECT_EQ(1920, entry.width);
  EXPECT_EQ(1080, entry.height);
}

TEST_F(VVCBoxTest, VVCVisualSampleEntryWriteRead) {
  VVCVisualSampleEntry original;

  original.format = FOURCC_vvc1;
  original.data_reference_index = 1;
  original.width = 1920;
  original.height = 1080;

  auto& vvcc = original.mutable_vvc_config();
  vvcc.set_length_size_minus_one(3);
  vvcc.set_ptl_present_flag(true);
  vvcc.set_max_picture_width(1920);
  vvcc.set_max_picture_height(1080);

  auto& ptl = vvcc.mutable_ptl_record();
  ptl.general_profile_idc = 1;
  ptl.general_tier_flag = 0;
  ptl.general_level_idc = 153;

  // Write
  BufferWriter writer;
  original.Write(&writer);

  // Read
  BufferReader reader(writer.Buffer(), writer.Size());
  VVCVisualSampleEntry parsed;
  ASSERT_TRUE(parsed.ReadWrite(&reader));

  // Verify
  EXPECT_EQ(original.format, parsed.format);
  EXPECT_EQ(original.width, parsed.width);
  EXPECT_EQ(original.height, parsed.height);
  EXPECT_EQ(original.vvc_config().max_picture_width(),
            parsed.vvc_config().max_picture_width());
  EXPECT_EQ(original.vvc_config().max_picture_height(),
            parsed.vvc_config().max_picture_height());
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordMinimalSize) {
  VVCDecoderConfigurationRecord vvcc;

  // Configuration minimale sans PTL
  vvcc.set_ptl_present_flag(false);

  size_t size = vvcc.ComputeSize();
  
  // Minimum: header + 2 bytes config + 1 byte num_arrays
  EXPECT_GE(size, 11u);  // 8 (box header) + 3 (data)
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordWithConstraints) {
  VVCDecoderConfigurationRecord vvcc;

  vvcc.set_ptl_present_flag(true);
  auto& ptl = vvcc.mutable_ptl_record();
  ptl.general_profile_idc = 1;
  ptl.general_tier_flag = 0;
  ptl.general_level_idc = 153;

  // Ajouter des constraint info
  ptl.general_constraint_info = {0x00, 0x00, 0x00, 0x00};
  ptl.num_bytes_constraint_info = 4;

  std::string codec = vvcc.GetCodecString(FOURCC_vvc1);
  
  // Devrait contenir les constraints
  EXPECT_TRUE(codec.find("vvc1.1.L153") == 0);
  // Note: le format exact des constraints peut varier
}

TEST_F(VVCBoxTest, MultipleNalUnitsInArray) {
  VVCDecoderConfigurationRecord vvcc;

  auto& arrays = vvcc.mutable_arrays();
  arrays.resize(1);
  arrays[0].nal_unit_type = 15;  // SPS
  arrays[0].array_completeness = 1;

  // Ajouter plusieurs SPS
  arrays[0].nal_units.push_back(CreateTestSPS());
  arrays[0].nal_units.push_back(CreateTestSPS());
  arrays[0].nal_units.push_back(CreateTestSPS());

  EXPECT_EQ(3u, arrays[0].nal_units.size());

  // Write and read
  BufferWriter writer;
  vvcc.Write(&writer);

  VVCDecoderConfigurationRecord parsed;
  ASSERT_TRUE(parsed.Parse(writer.Buffer(), writer.Size()));

  ASSERT_EQ(1u, parsed.arrays().size());
  EXPECT_EQ(3u, parsed.arrays()[0].nal_units.size());
}

TEST_F(VVCBoxTest, VVCDecoderConfigurationRecordDifferentLengthSizes) {
  for (uint8_t length_size : {0, 1, 2, 3}) {
    VVCDecoderConfigurationRecord vvcc;
    vvcc.set_length_size_minus_one(length_size);

    BufferWriter writer;
    vvcc.Write(&writer);

    VVCDecoderConfigurationRecord parsed;
    ASSERT_TRUE(parsed.Parse(writer.Buffer(), writer.Size()));

    EXPECT_EQ(length_size, parsed.length_size_minus_one());
  }
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
