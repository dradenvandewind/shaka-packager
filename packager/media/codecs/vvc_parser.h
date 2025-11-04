// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CODECS_VVC_PARSER_H_
#define PACKAGER_MEDIA_CODECS_VVC_PARSER_H_

#include <cstdint>
#include <vector>

namespace shaka {
namespace media {

/// Parser for VVC NAL units (H.266)
/// Handles VVC bitstream parsing according to ISO/IEC 23090-3
class VvcParser {
 public:
  /// VVC NAL unit types
  enum NalUnitType {
    // VCL NAL units
    kTrail = 0,
    kStsa = 1,
    kRadl = 2,
    kRasl = 3,
    kIdrWRadl = 7,
    kIdrNLp = 8,
    kCra = 9,
    kGdr = 10,
    
    // Non-VCL NAL units
    kOpi = 12,        // Operating point information
    kDci = 13,        // Decoding capability information
    kVps = 14,        // Video parameter set
    kSps = 15,        // Sequence parameter set
    kPps = 16,        // Picture parameter set
    kPrefixAps = 17,  // Adaptation parameter set
    kSuffixAps = 18,  // Adaptation parameter set (suffix)
    kPh = 19,         // Picture header
    kAud = 20,        // Access unit delimiter
    kEos = 21,        // End of sequence
    kEob = 22,        // End of bitstream
    kPrefixSei = 23,  // Supplemental enhancement information
    kSuffixSei = 24,  // Supplemental enhancement information (suffix)
    kFd = 25,         // Filler data
    
    kReservedNvcl26 = 26,
    kReservedNvcl27 = 27,
    kUnspec28 = 28,
    kUnspec29 = 29,
    kUnspec30 = 30,
    kUnspec31 = 31,
  };

  /// Structure representing a VVC NAL unit
  struct NalUnit {
    NalUnitType type;
    uint8_t layer_id;
    uint8_t temporal_id;
    const uint8_t* data;  // Pointer to data (without start code)
    size_t size;          // Data size
    
    /// Indicates if it's a VCL NAL unit (Video Coding Layer)
    bool IsVcl() const {
      return type <= kGdr;
    }
    
    /// Indicates if it's a parameter set
    bool IsParameterSet() const {
      return type == kVps || type == kSps || type == kPps || 
             type == kPrefixAps || type == kSuffixAps;
    }
    
    /// Indicates if it's an IDR (Instantaneous Decoder Refresh)
    bool IsIdr() const {
      return type == kIdrWRadl || type == kIdrNLp;
    }
    
    /// Indicates if it's a keyframe (IRAP - Intra Random Access Point)
    bool IsKeyframe() const {
      return IsIdr() || type == kCra;
    }
  };

  VvcParser();
  ~VvcParser();

  /// Parse NAL units from a buffer (Annex B format with start codes)
  /// @param data Buffer containing NAL units
  /// @param size Buffer size
  /// @param nal_units Output vector containing parsed NAL units
  /// @return true if parsing succeeded
  bool ParseNalUnits(const uint8_t* data,
                     size_t size,
                     std::vector<NalUnit>* nal_units);

  /// Parse a single NAL unit
  /// @param data NAL unit data (without start code)
  /// @param size Data size
  /// @param nal_unit Output structure
  /// @return true if parsing succeeded
  bool ParseNalUnit(const uint8_t* data,
                    size_t size,
                    NalUnit* nal_unit);

  /// Extract VVC NAL unit header (2 bytes)
  /// @param data NAL unit data
  /// @param size Data size
  /// @param type NAL unit type (output)
  /// @param layer_id Layer ID (output)
  /// @param temporal_id Temporal ID (output)
  /// @return true if extraction succeeded
  static bool ExtractNalUnitHeader(const uint8_t* data,
                                   size_t size,
                                   uint8_t* type,
                                   uint8_t* layer_id,
                                   uint8_t* temporal_id);

  /// Find VVC start codes (0x000001 or 0x00000001)
  /// @param data Buffer to analyze
  /// @param size Buffer size
  /// @param start_codes Positions of found start codes
  /// @return Number of start codes found
  static size_t FindStartCodes(const uint8_t* data,
                               size_t size,
                               std::vector<size_t>* start_codes);

  /// Convert from Annex B format (with start codes) to length-prefixed format
  /// @param data Annex B buffer
  /// @param size Buffer size
  /// @param length_size Length field size (1, 2 or 4 bytes)
  /// @param output Output buffer
  /// @return true if conversion succeeded
  static bool ConvertAnnexBToLengthPrefixed(const uint8_t* data,
                                           size_t size,
                                           size_t length_size,
                                           std::vector<uint8_t>* output);

  /// Convert from length-prefixed format to Annex B format
  /// @param data Length-prefixed buffer
  /// @param size Buffer size
  /// @param length_size Length field size
  /// @param output Output buffer
  /// @return true if conversion succeeded
  static bool ConvertLengthPrefixedToAnnexB(const uint8_t* data,
                                           size_t size,
                                           size_t length_size,
                                           std::vector<uint8_t>* output);

  /// Remove emulation prevention bytes (0x03)
  /// @param data Input buffer
  /// @param size Buffer size
  /// @param output Output buffer
  static void RemoveEmulationPrevention(const uint8_t* data,
                                       size_t size,
                                       std::vector<uint8_t>* output);

  /// Add emulation prevention bytes
  /// @param data Input buffer
  /// @param size Buffer size
  /// @param output Output buffer
  static void AddEmulationPrevention(const uint8_t* data,
                                    size_t size,
                                    std::vector<uint8_t>* output);

 private:
  // Detect a start code at a given position
  static bool IsStartCode(const uint8_t* data, size_t size, size_t pos, size_t* code_size);
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_VVC_PARSER_H_