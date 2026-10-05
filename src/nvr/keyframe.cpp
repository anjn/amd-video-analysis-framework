#include "keyframe.hpp"
namespace amdva::nvr {
KeyframeCache::~KeyframeCache() {
    gst_h264_nal_parser_free(h264_);
    gst_h265_parser_free(h265_);
}
void KeyframeCache::reset() {
    parameters_.clear();
    gst_h264_nal_parser_free(h264_);
    gst_h265_parser_free(h265_);
    h264_ = gst_h264_nal_parser_new();
    h265_ = gst_h265_parser_new();
    codec_ = 0;
}
bool KeyframeCache::observe(GstBuffer *buffer, int codec) {
    type_ = -1;
    if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED))
        return false;
    if (codec != 1 && codec != 2)
        return false;
    if (codec_ != codec) {
        reset();
        codec_ = codec;
    }
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ))
        return false;
    unsigned offset = 0;
    bool picture = false, valid = true;
    while (offset < map.size) {
        unsigned type = 0, id = 0, start = 0, size = 0;
        bool parameter = false;
        if (codec == 1) {
            GstH264NalUnit n{};
            const auto result =
                gst_h264_parser_identify_nalu(h264_, map.data, offset, map.size, &n);
            if (result != GST_H264_PARSER_OK && result != GST_H264_PARSER_NO_NAL_END) {
                valid = false;
                break;
            }
            type = n.type;
            start = n.offset;
            size = n.size;
            parameter = type == GST_H264_NAL_SPS || type == GST_H264_NAL_PPS;
            if (parameter) {
                if (gst_h264_parser_parse_nal(h264_, &n) != GST_H264_PARSER_OK) {
                    valid = false;
                    break;
                }
                id = type == GST_H264_NAL_SPS ? h264_->last_sps->id : h264_->last_pps->id;
            } else if (type >= 1 && type <= 5) {
                GstH264SliceHdr slice{};
                picture = true;
                type_ = type;
                if (type != GST_H264_NAL_SLICE_IDR ||
                    gst_h264_parser_parse_slice_hdr(h264_, &n, &slice, FALSE, FALSE) !=
                        GST_H264_PARSER_OK)
                    valid = false;
            }
        } else {
            GstH265NalUnit n{};
            const auto result =
                gst_h265_parser_identify_nalu(h265_, map.data, offset, map.size, &n);
            if (result != GST_H265_PARSER_OK && result != GST_H265_PARSER_NO_NAL_END) {
                valid = false;
                break;
            }
            type = n.type;
            start = n.offset;
            size = n.size;
            if (n.layer_id != 0)
                valid = false;
            parameter = type >= GST_H265_NAL_VPS && type <= GST_H265_NAL_PPS;
            if (parameter) {
                if (gst_h265_parser_parse_nal(h265_, &n) != GST_H265_PARSER_OK) {
                    valid = false;
                    break;
                }
                id = type == GST_H265_NAL_VPS   ? h265_->last_vps->id
                     : type == GST_H265_NAL_SPS ? h265_->last_sps->id
                                                : h265_->last_pps->id;
            } else if (type <= 31) {
                GstH265SliceHdr slice{};
                picture = true;
                type_ = type;
                if (!(GST_H265_IS_NAL_TYPE_IDR(type) || GST_H265_IS_NAL_TYPE_BLA(type) ||
                      GST_H265_IS_NAL_TYPE_CRA(type)) ||
                    gst_h265_parser_parse_slice_hdr(h265_, &n, &slice) != GST_H265_PARSER_OK)
                    valid = false;
            }
        }
        if (!size || start + size > map.size || start + size <= offset) {
            valid = false;
            break;
        }
        if (parameter) {
            // Bound retained headers even for malicious camera streams.
            auto bytes = std::vector<uint8_t>{0, 0, 0, 1};
            bytes.insert(bytes.end(), map.data + start, map.data + start + size);
            parameters_[{type, id}] = std::move(bytes);
            size_t total = 0;
            for (const auto &p : parameters_)
                total += p.second.size();
            if (total > (1u << 20)) {
                parameters_.clear();
                valid = false;
                break;
            }
        }
        if (codec == 1 && (type == 19 || type == 20 || type == 21))
            valid = false;
        offset = start + size;
    }
    gst_buffer_unmap(buffer, &map);
    return valid && picture;
}
GstBuffer *KeyframeCache::input(GstBuffer *au) const {
    size_t size = 0;
    for (const auto &p : parameters_)
        size += p.second.size();
    if (!size)
        return nullptr;
    auto *headers = gst_buffer_new_allocate(nullptr, size, nullptr);
    size_t offset = 0;
    for (const auto &p : parameters_) {
        gst_buffer_fill(headers, offset, p.second.data(), p.second.size());
        offset += p.second.size();
    }
    auto *result = gst_buffer_append(headers, gst_buffer_copy(au));
    GST_BUFFER_PTS(result) = 0;
    GST_BUFFER_DTS(result) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(result) = GST_BUFFER_DURATION(au);
    GST_BUFFER_FLAG_SET(result, GST_BUFFER_FLAG_DISCONT);
    GST_BUFFER_FLAG_UNSET(result, GST_BUFFER_FLAG_DELTA_UNIT);
    return result;
}
} // namespace amdva::nvr
