#ifndef PACKAGER_MEDIA_CODECS_H266_BYTE_TO_UNIT_STREAM_CONVERTER_H_
#define PACKAGER_MEDIA_CODECS_H266_BYTE_TO_UNIT_STREAM_CONVERTER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include <packager/macros/classes.h>
#include <packager/media/codecs/h26x_byte_to_unit_stream_converter.h>


namespace shaka {
namespace media {

class H266ByteToUnitStreamConverter : public H26xByteToUnitStreamConverter {
 public:
  H266ByteToUnitStreamConverter();
  explicit H266ByteToUnitStreamConverter(H26xStreamFormat stream_format);

  ~H266ByteToUnitStreamConverter() override;

  /// @name H26xByteToUnitStreamConverter implementation override.
  /// @{
  bool GetDecoderConfigurationRecord(
      std::vector<uint8_t>* decoder_config) const override;
  /// @}


 private:
  bool ProcessNalu(const Nalu& nalu) override;

  std::vector<uint8_t> last_vps_;
  std::vector<uint8_t> last_sps_;
  std::vector<uint8_t> last_pps_;
  std::vector<uint8_t> last_dci_;
  std::vector<uint8_t> last_opi_;
  
 DISALLOW_COPY_AND_ASSIGN(H266ByteToUnitStreamConverter);

};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CODECS_H266_BYTE_TO_UNIT_STREAM_CONVERTER_H_