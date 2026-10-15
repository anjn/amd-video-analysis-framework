#include "color.hpp"
#include <array>
#include <cstring>
#include <gst/codecparsers/gsth264bitwriter.h>
#include <iostream>
#include <stdexcept>
using namespace amdva::dashboard;
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
int main() {
    gst_init(nullptr, nullptr);
    try {
        GstH264SPS input{};
        input.profile_idc = 100;
        input.level_idc = 40;
        input.id = 0;
        input.chroma_format_idc = 1;
        input.pic_order_cnt_type = 0;
        input.log2_max_pic_order_cnt_lsb_minus4 = 2;
        input.num_ref_frames = 1;
        input.pic_width_in_mbs_minus1 = 119;
        input.pic_height_in_map_units_minus1 = 67;
        input.frame_mbs_only_flag = 1;
        input.direct_8x8_inference_flag = 1;
        input.frame_cropping_flag = 1;
        input.frame_crop_bottom_offset = 4;
        input.vui_parameters_present_flag = 1;
        input.vui_parameters.timing_info_present_flag = 1;
        input.vui_parameters.num_units_in_tick = 1;
        input.vui_parameters.time_scale = 20;
        input.vui_parameters.fixed_frame_rate_flag = 1;
        std::array<guint8, 4096> raw{}, nal{};
        guint raw_size = raw.size(), size = nal.size();
        check(gst_h264_bit_writer_sps(&input, TRUE, raw.data(), &raw_size) ==
                  GST_H264_BIT_WRITER_OK,
              "Fixture SPS writing");
        check(gst_h264_bit_writer_convert_to_nal(4, FALSE, TRUE, FALSE, raw.data(), raw_size * 8,
                                                 nal.data(), &size) == GST_H264_BIT_WRITER_OK,
              "Fixture SPS escaping");
        const unsigned char aud[] = {0, 0, 0, 1, 9, 0xf0};
        std::memcpy(nal.data() + size, aud, sizeof(aud));
        size += sizeof(aud);
        auto *source = gst_buffer_new_allocate(nullptr, size, nullptr);
        gst_buffer_fill(source, 0, nal.data(), size);
        GST_BUFFER_PTS(source) = 123456;
        auto *out = signal_bt709(source);
        check(GST_BUFFER_PTS(out) == 123456, "Encoded timing metadata lost");
        GstMapInfo map{};
        check(gst_buffer_map(out, &map, GST_MAP_READ), "Result map");
        auto *parser = gst_h264_nal_parser_new();
        GstH264NalUnit nalu{};
        check(gst_h264_parser_identify_nalu(parser, map.data, 0, map.size, &nalu) ==
                  GST_H264_PARSER_OK,
              "Result NAL");
        GstH264SPS decoded{};
        check(gst_h264_parse_sps(&nalu, &decoded) == GST_H264_PARSER_OK, "Result SPS");
        check(decoded.vui_parameters.video_signal_type_present_flag &&
                  decoded.vui_parameters.colour_description_present_flag &&
                  !decoded.vui_parameters.video_full_range_flag &&
                  decoded.vui_parameters.colour_primaries == 1 &&
                  decoded.vui_parameters.transfer_characteristics == 1 &&
                  decoded.vui_parameters.matrix_coefficients == 1,
              "BT709 limited signaling");
        check(decoded.pic_width_in_mbs_minus1 == 119 && decoded.frame_crop_bottom_offset == 4 &&
                  decoded.vui_parameters.time_scale == 20,
              "Dimensions/cropping/timing changed");
        check(map.size >= sizeof(aud) &&
                  !std::memcmp(map.data + map.size - sizeof(aud), aud, sizeof(aud)),
              "Non-SPS NAL changed");
        gst_h264_sps_clear(&decoded);
        gst_h264_nal_parser_free(parser);
        gst_buffer_unmap(out, &map);
        gst_buffer_unref(out);
        gst_buffer_unref(source);
        source = gst_buffer_new_allocate(nullptr, sizeof(aud), nullptr);
        gst_buffer_fill(source, 0, aud, sizeof(aud));
        out = signal_bt709(source);
        check(source == out, "No-SPS AU should retain buffer");
        gst_buffer_unref(out);
        gst_buffer_unref(source);
        const unsigned char bad[] = {0, 0, 1, 0x67, 0xff};
        source = gst_buffer_new_allocate(nullptr, sizeof(bad), nullptr);
        gst_buffer_fill(source, 0, bad, sizeof(bad));
        bool rejected = false;
        try {
            out = signal_bt709(source);
            gst_buffer_unref(out);
        } catch (...) {
            rejected = true;
        }
        gst_buffer_unref(source);
        check(rejected, "Malformed SPS accepted");
        std::cout << "BT709 limited SPS, frame geometry/timing, non-SPS preservation and malformed "
                     "input passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
