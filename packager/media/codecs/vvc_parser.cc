// Copyright 2024 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "packager/media/codecs/vvc_parser.h"

#include <algorithm>
#include <cstring>

#include "packager/base/logging.h"

namespace shaka {
namespace media {

namespace {

// Start codes VVC
const uint8_t kStartCode3[] = {0x00, 0x00, 0x01};
const uint8_t kStartCode4[] = {0x00, 0x00, 0x00, 0x01};

}  // namespace

H266Parser ::H266Parser () = default;
H266Parser ::~H266Parser () = default;

bool H266Parser ::ParseNalUnits(const uint8_t* data,
                              size_t size,
                              std::vector<NalUnit>* nal_units) {
  if (!data || size == 0 || !nal_units) {
    LOG(ERROR) << "Invalid input parameters";
    return false;
  }

  nal_units->clear();

  // Trouver tous les start codes
  std::vector<size_t> start_codes;
  FindStartCodes(data, size, &start_codes);

  if (start_codes.empty()) {
    // Pas de start code trouvé, essayer de parser comme une seule NAL unit
    NalUnit nalu;
    if (ParseNalUnit(data, size, &nalu)) {
      nal_units->push_back(nalu);
      return true;
    }
    LOG(WARNING) << "No start codes found and failed to parse as single NAL unit";
    return false;
  }

  // Parser chaque NAL unit entre les start codes
  for (size_t i = 0; i < start_codes.size(); i++) {
    size_t start_code_pos = start_codes[i];
    
    // Déterminer la taille du start code (3 ou 4 bytes)
    size_t start_code_size = 3;
    if (start_code_pos > 0 && data[start_code_pos - 1] == 0x00) {
      start_code_size = 4;
      start_code_pos--;
    }

    // Position du début des données NAL (après le start code)
    size_t nal_start = start_code_pos + start_code_size;
    
    // Trouver la fin de cette NAL unit
    size_t nal_end;
    if (i + 1 < start_codes.size()) {
      nal_end = start_codes[i + 1];
      // Reculer si le prochain start code est de 4 bytes
      if (nal_end > 0 && data[nal_end - 1] == 0x00) {
        nal_end--;
      }
    } else {
      nal_end = size;
    }

    // Vérifier la validité
    if (nal_end <= nal_start) {
      LOG(WARNING) << "Invalid NAL unit boundaries at position " << start_code_pos;
      continue;
    }

    size_t nal_size = nal_end - nal_start;
    
    // Parser cette NAL unit
    NalUnit nalu;
    if (ParseNalUnit(data + nal_start, nal_size, &nalu)) {
      nal_units->push_back(nalu);
    } else {
      LOG(WARNING) << "Failed to parse NAL unit at position " << nal_start;
    }
  }

  return !nal_units->empty();
}

bool H266Parser ::ParseNalUnit(const uint8_t* data,
                             size_t size,
                             NalUnit* nal_unit) {
  if (!data || size < 2 || !nal_unit) {
    LOG(ERROR) << "Invalid NAL unit data (size=" << size << ")";
    return false;
  }

  uint8_t type, layer_id, temporal_id;
  if (!ExtractNalUnitHeader(data, size, &type, &layer_id, &temporal_id)) {
    LOG(ERROR) << "Failed to extract NAL unit header";
    return false;
  }

  // Valider le type de NAL unit
  if (type > kUnspec31) {
    LOG(WARNING) << "Unknown NAL unit type: " << static_cast<int>(type);
  }

  nal_unit->type = static_cast<NalUnitType>(type);
  nal_unit->layer_id = layer_id;
  nal_unit->temporal_id = temporal_id;
  nal_unit->data = data;
  nal_unit->size = size;

  return true;
}

bool H266Parser ::ExtractNalUnitHeader(const uint8_t* data,
                                     size_t size,
                                     uint8_t* type,
                                     uint8_t* layer_id,
                                     uint8_t* temporal_id) {
  if (!data || size < 2 || !type || !layer_id || !temporal_id) {
    return false;
  }

  // NAL unit header VVC (2 bytes):
  // Byte 0: forbidden_zero_bit(1) + nuh_reserved_zero_bit(1) + nuh_layer_id[5:4](2) + nal_unit_type[4:1](4)
  // Byte 1: nal_unit_type[0](1) + nuh_temporal_id_plus1(3) + nuh_layer_id[3:0](4)
  
  // Format réel selon ISO/IEC 23090-3:
  // forbidden_zero_bit(1) + nuh_reserved_zero_bit(1) + nuh_layer_id(6) + nal_unit_type(5) + nuh_temporal_id_plus1(3)

  uint16_t header = (static_cast<uint16_t>(data[0]) << 8) | data[1];

  // Extraire forbidden_zero_bit (bit 15)
  if (header & 0x8000) {
    LOG(WARNING) << "VVC NAL unit has forbidden_zero_bit set";
    return false;
  }

  // Extraire nuh_reserved_zero_bit (bit 14) - devrait être 0 mais on le tolère
  bool reserved_bit = (header & 0x4000) != 0;
  if (reserved_bit) {
    VLOG(2) << "VVC NAL unit has reserved bit set (tolerated)";
  }

  // Extraire nuh_layer_id (bits 13-8, 6 bits)
  *layer_id = (header >> 8) & 0x3F;

  // Extraire nal_unit_type (bits 7-3, 5 bits)
  *type = (header >> 3) & 0x1F;

  // Extraire nuh_temporal_id_plus1 (bits 2-0, 3 bits)
  uint8_t temporal_id_plus1 = header & 0x07;

  // Valider temporal_id_plus1
  if (temporal_id_plus1 == 0) {
    LOG(ERROR) << "VVC NAL unit has invalid nuh_temporal_id_plus1 = 0";
    return false;
  }

  *temporal_id = temporal_id_plus1 - 1;

  return true;
}

size_t H266Parser ::FindStartCodes(const uint8_t* data,
                                 size_t size,
                                 std::vector<size_t>* start_codes) {
  if (!data || size < 3 || !start_codes) {
    return 0;
  }

  start_codes->clear();

  // Recherche optimisée des start codes
  for (size_t i = 0; i <= size - 3; i++) {
    size_t code_size;
    if (IsStartCode(data, size, i, &code_size)) {
      start_codes->push_back(i);
      i += code_size - 1;  // Sauter le start code
    }
  }

  return start_codes->size();
}

bool H266Parser ::IsStartCode(const uint8_t* data,
                           size_t size,
                           size_t pos,
                           size_t* code_size) {
  // Vérifier le start code 4 bytes (0x00000001)
  if (pos + 3 < size &&
      data[pos] == 0x00 &&
      data[pos + 1] == 0x00 &&
      data[pos + 2] == 0x00 &&
      data[pos + 3] == 0x01) {
    *code_size = 4;
    return true;
  }

  // Vérifier le start code 3 bytes (0x000001)
  if (pos + 2 < size &&
      data[pos] == 0x00 &&
      data[pos + 1] == 0x00 &&
      data[pos + 2] == 0x01) {
    *code_size = 3;
    return true;
  }

  return false;
}

bool H266Parser ::ConvertAnnexBToLengthPrefixed(const uint8_t* data,
                                             size_t size,
                                             size_t length_size,
                                             std::vector<uint8_t>* output) {
  if (!data || size == 0 || !output) {
    LOG(ERROR) << "Invalid parameters for Annex B conversion";
    return false;
  }

  if (length_size != 1 && length_size != 2 && length_size != 4) {
    LOG(ERROR) << "Invalid length_size: " << length_size 
               << " (must be 1, 2, or 4)";
    return false;
  }

  output->clear();
  output->reserve(size);  // Estimation conservatrice

  std::vector<NalUnit> nal_units;
  H266Parser  parser;
  if (!parser.ParseNalUnits(data, size, &nal_units)) {
    LOG(ERROR) << "Failed to parse NAL units from Annex B data";
    return false;
  }

  for (const auto& nalu : nal_units) {
    uint32_t nal_size = static_cast<uint32_t>(nalu.size);
    
    // Vérifier que la taille ne dépasse pas la capacité du length field
    if (length_size == 1 && nal_size > 0xFF) {
      LOG(ERROR) << "NAL unit too large for 1-byte length: " << nal_size;
      return false;
    }
    if (length_size == 2 && nal_size > 0xFFFF) {
      LOG(ERROR) << "NAL unit too large for 2-byte length: " << nal_size;
      return false;
    }

    // Écrire le length field (big-endian)
    if (length_size == 4) {
      output->push_back((nal_size >> 24) & 0xFF);
      output->push_back((nal_size >> 16) & 0xFF);
      output->push_back((nal_size >> 8) & 0xFF);
      output->push_back(nal_size & 0xFF);
    } else if (length_size == 2) {
      output->push_back((nal_size >> 8) & 0xFF);
      output->push_back(nal_size & 0xFF);
    } else {  // length_size == 1
      output->push_back(nal_size & 0xFF);
    }

    // Écrire les données de la NAL unit
    output->insert(output->end(), nalu.data, nalu.data + nalu.size);
  }

  VLOG(2) << "Converted " << nal_units.size() << " NAL units from Annex B to length-prefixed";
  return true;
}

bool H266Parser ::ConvertLengthPrefixedToAnnexB(const uint8_t* data,
                                             size_t size,
                                             size_t length_size,
                                             std::vector<uint8_t>* output) {
  if (!data || size == 0 || !output) {
    LOG(ERROR) << "Invalid parameters for length-prefixed conversion";
    return false;
  }

  if (length_size != 1 && length_size != 2 && length_size != 4) {
    LOG(ERROR) << "Invalid length_size: " << length_size;
    return false;
  }

  output->clear();
  output->reserve(size + size / 10);  // Estimation: +10% pour start codes

  size_t pos = 0;
  int nal_count = 0;

  while (pos + length_size <= size) {
    // Lire le length field (big-endian)
    uint32_t nal_size = 0;
    if (length_size == 4) {
      nal_size = (static_cast<uint32_t>(data[pos]) << 24) |
                 (static_cast<uint32_t>(data[pos + 1]) << 16) |
                 (static_cast<uint32_t>(data[pos + 2]) << 8) |
                 static_cast<uint32_t>(data[pos + 3]);
    } else if (length_size == 2) {
      nal_size = (static_cast<uint32_t>(data[pos]) << 8) |
                 static_cast<uint32_t>(data[pos + 1]);
    } else {  // length_size == 1
      nal_size = data[pos];
    }

    pos += length_size;

    // Vérifier que la NAL unit ne dépasse pas le buffer
    if (pos + nal_size > size) {
      LOG(ERROR) << "Invalid NAL unit size " << nal_size 
                 << " at position " << (pos - length_size)
                 << " (exceeds buffer)";
      return false;
    }

    // Écrire le start code (4 bytes pour la première NAL, 3 bytes ensuite)
    if (nal_count == 0) {
      output->insert(output->end(), kStartCode4, kStartCode4 + 4);
    } else {
      output->insert(output->end(), kStartCode3, kStartCode3 + 3);
    }

    // Écrire les données de la NAL unit
    output->insert(output->end(), data + pos, data + pos + nal_size);

    pos += nal_size;
    nal_count++;
  }

  if (pos != size) {
    LOG(ERROR) << "Incomplete NAL units in buffer: processed " << pos 
               << " bytes out of " << size;
    return false;
  }

  VLOG(2) << "Converted " << nal_count << " NAL units from length-prefixed to Annex B";
  return true;
}

void H266Parser ::RemoveEmulationPrevention(const uint8_t* data,
                                         size_t size,
                                         std::vector<uint8_t>* output) {
  if (!data || size == 0 || !output) {
    return;
  }

  output->clear();
  output->reserve(size);  // Maximum size (si pas d'emulation prevention)

  size_t i = 0;
  while (i < size) {
    // Chercher la séquence 0x000003
    if (i + 2 < size &&
        data[i] == 0x00 &&
        data[i + 1] == 0x00 &&
        data[i + 2] == 0x03) {
      
      // Vérifier que le byte suivant justifie l'emulation prevention
      if (i + 3 < size &&
          (data[i + 3] == 0x00 || data[i + 3] == 0x01 ||
           data[i + 3] == 0x02 || data[i + 3] == 0x03)) {
        // C'est un vrai emulation prevention byte
        output->push_back(0x00);
        output->push_back(0x00);
        // Sauter le 0x03
        i += 3;
      } else {
        // Pas un emulation prevention byte valide
        output->push_back(data[i]);
        i++;
      }
    } else {
      output->push_back(data[i]);
      i++;
    }
  }
}

void H266Parser::AddEmulationPrevention(const uint8_t* data,
                                      size_t size,
                                      std::vector<uint8_t>* output) {
  if (!data || size == 0 || !output) {
    return;
  }

  output->clear();
  output->reserve(size + size / 100);  // Estimation: ~1% d'augmentation

  size_t zero_count = 0;

  for (size_t i = 0; i < size; i++) {
    uint8_t byte = data[i];

    // Compter les zéros consécutifs
    if (byte == 0x00) {
      zero_count++;
      output->push_back(byte);
    } else {
      // Si on a deux zéros suivis de 0x00, 0x01, 0x02 ou 0x03,
      // on doit avoir inséré un emulation prevention byte avant
      if (zero_count >= 2 && (byte <= 0x03)) {
        // Insérer l'emulation prevention byte
        output->push_back(0x03);
      }
      
      output->push_back(byte);
      zero_count = 0;
    }
  }
}

}  // namespace media
}  // namespace shaka