#include "amdva/vui.hpp"
#include <array>
#include <gst/codecparsers/gsth264bitwriter.h>
#include <gst/codecparsers/gsth264parser.h>
#include <memory>
#include <stdexcept>
#include <vector>
namespace amdva {
GstBuffer *signal_bt709(GstBuffer *input) {
    GstMapInfo map{};
    if (!gst_buffer_map(input, &map, GST_MAP_READ))
        throw std::runtime_error("Cannot read encoded H264 AU");
    std::unique_ptr<GstH264NalParser, decltype(&gst_h264_nal_parser_free)> parser(
        gst_h264_nal_parser_new(), gst_h264_nal_parser_free);
    std::vector<uint8_t> bytes;
    bool changed = false;
    try {
        size_t offset = 0;
        while (offset < map.size) {
            GstH264NalUnit nalu{};
            auto result =
                gst_h264_parser_identify_nalu(parser.get(), map.data, offset, map.size, &nalu);
            if (result != GST_H264_PARSER_OK && result != GST_H264_PARSER_NO_NAL_END)
                throw std::runtime_error("Invalid encoded H264 NAL boundary");
            if (nalu.offset + nalu.size > map.size || nalu.offset + nalu.size <= offset)
                throw std::runtime_error("Invalid encoded H264 NAL size");
            if (nalu.sc_offset > offset)
                bytes.insert(bytes.end(), map.data + offset, map.data + nalu.sc_offset);
            if (nalu.type == GST_H264_NAL_SPS) {
                GstH264SPS sps{};
                if (gst_h264_parse_sps(&nalu, &sps) != GST_H264_PARSER_OK)
                    throw std::runtime_error("Cannot parse encoded H264 SPS");
                sps.vui_parameters_present_flag = 1;
                auto &vui = sps.vui_parameters;
                vui.video_signal_type_present_flag = 1;
                vui.video_format = 5;
                vui.video_full_range_flag = 0;
                vui.colour_description_present_flag = 1;
                vui.colour_primaries = 1;
                vui.transfer_characteristics = 1;
                vui.matrix_coefficients = 1;
                std::array<guint8, 16384> raw{}, nal{};
                guint raw_size = raw.size(), nal_size = nal.size();
                const auto written = gst_h264_bit_writer_sps(&sps, TRUE, raw.data(), &raw_size);
                gst_h264_sps_clear(&sps);
                if (written != GST_H264_BIT_WRITER_OK ||
                    gst_h264_bit_writer_convert_to_nal(4, FALSE, TRUE, FALSE, raw.data(),
                                                       raw_size * 8, nal.data(),
                                                       &nal_size) != GST_H264_BIT_WRITER_OK)
                    throw std::runtime_error("Cannot signal BT709 H264 SPS");
                bytes.insert(bytes.end(), nal.begin(), nal.begin() + nal_size);
                changed = true;
            } else
                bytes.insert(bytes.end(), map.data + nalu.sc_offset,
                             map.data + nalu.offset + nalu.size);
            offset = nalu.offset + nalu.size;
        }
    } catch (...) {
        gst_buffer_unmap(input, &map);
        throw;
    }
    gst_buffer_unmap(input, &map);
    if (!changed)
        return gst_buffer_ref(input);
    auto *out = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
    gst_buffer_fill(out, 0, bytes.data(), bytes.size());
    gst_buffer_copy_into(out, input, GST_BUFFER_COPY_METADATA, 0, -1);
    return out;
}
} // namespace amdva
