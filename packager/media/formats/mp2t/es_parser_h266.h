// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_FORMATS_MP2T_ES_PARSER_H266_H_
#define PACKAGER_MEDIA_FORMATS_MP2T_ES_PARSER_H266_H_

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <utility>

#include <packager/macros/classes.h>
#include <packager/media/formats/mp2t/es_parser_h26x.h>
#include <packager/media/codecs/h266_parser.h>

#include <functional>

namespace shaka {
namespace media {

class H266Parser;

namespace mp2t {

class EsParserH266 : public EsParserH26x {
 public:
  EsParserH266(uint32_t pid,
               const NewStreamInfoCB& new_stream_info_cb,
               const EmitSampleCB& emit_sample_cb);
  ~EsParserH266() override;

  // EsParserH26x implementation override.
  void Reset() override;

 private:
  EsParserH266(const EsParserH266&) = delete;
  EsParserH266& operator=(const EsParserH266&) = delete;

  // Processes VCL NALUs and updates video slice info.
  bool ProcessVclNalu(const Nalu& nalu, VideoSliceInfo* video_slice_info);

  // Processes non-VCL NALUs (SPS, PPS, VPS, etc.).
  bool ProcessOtherNonVclNalu(const Nalu& nalu);

  // Processes a NAL unit found in ParseInternal.
  bool ProcessNalu(const Nalu& nalu, VideoSliceInfo* video_slice_info) override;

  // Update the video decoder config based on an H264 SPS.
  // Return true if successful.
  bool UpdateVideoDecoderConfig(int sps_id) override;

  int64_t CalculateSampleDuration(int pps_id) override;
  // Callback to pass the stream configuration.
  NewStreamInfoCB new_stream_info_cb_;

  

  int64_t GetSampleDurationFromSps(int pps_id);
  int64_t CalculateDurationFromRecentTimestamps();
  int64_t GetDefaultSampleDuration();
  std::shared_ptr<H266Sps> GetSpsForPps(int pps_id);
  std::shared_ptr<H266Sps> GetLastActiveSps();


   struct TimestampEntry {
    int64_t pts;
    int64_t dts;
    int64_t duration;
  };
  std::deque<TimestampEntry> timestamp_tracker_;
  double last_frame_rate_ = 0.0;
  int64_t last_sample_duration_ = 0;

  std::map<int, std::shared_ptr<H266Pps>> pps_map_;
  std::map<int, std::shared_ptr<H266Sps>> sps_map_;
  std::map<int, std::shared_ptr<H266Vps>> vps_map_;

  std::shared_ptr<H266Pps> last_pps_;
  std::shared_ptr<H266Sps> last_sps_;
  std::shared_ptr<H266Vps> last_vps_;

  std::vector<uint8_t> current_access_unit_;
  int64_t current_access_unit_pts_ = -1;
  int64_t current_access_unit_dts_ = -1;
  bool current_access_unit_is_keyframe_ = false;

  bool waiting_for_keyframe_ = true;
  
  // Statistiques et métriques
  size_t frames_parsed_ = 0;
  int64_t first_pts_ = -1;
  int64_t last_pts_ = -1;

  // Buffer pour accumulation des données
  std::vector<uint8_t> es_buffer_;

  std::vector<std::shared_ptr<MediaSample>> pending_samples_;
  //  std::unique_ptr<VideoStreamInfo> video_stream_info_;





  // Last video decoder config.
  std::shared_ptr<StreamInfo> last_video_decoder_config_;
  bool decoder_config_check_pending_;
  std::unique_ptr<H266Parser> parser_;
  int previous_nalu_type_ = -1;
};

}  // namespace mp2t
}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_FORMATS_MP2T_ES_PARSER_H266_H_
