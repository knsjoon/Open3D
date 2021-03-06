// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/t/io/sensor/realsense/RSBagReader.h"

#include <json/json.h>

#include <chrono>
#include <iomanip>
#include <iostream>
#include <librealsense2/rs.hpp>
#include <mutex>
#include <sstream>

namespace open3d {
namespace t {
namespace io {

RSBagReader::RSBagReader(size_t buffer_size)
    : frame_buffer_(buffer_size),
      frame_position_us_(buffer_size),
      pipe_(new rs2::pipeline),
      align_to_color_(new rs2::align(rs2_stream::RS2_STREAM_COLOR)) {}

RSBagReader::~RSBagReader() {
    if (IsOpened()) Close();
}

bool RSBagReader::Open(const std::string &filename) {
    return Open(filename, 0);
}

bool RSBagReader::Open(const std::string &filename, uint64_t start_time_us) {
    if (IsOpened()) {
        Close();
    }
    try {
        rs2::config cfg;
        cfg.enable_device_from_file(filename, false);  // Do not repeat playback
        pipe_->start(cfg);  // File will be opened in read mode at this point
        // do not drop frames: Causes deadlock after 4 frames on macOS/Linux
        // https://github.com/IntelRealSense/librealsense/issues/7547#issuecomment-706984376
        /* rs_device.set_real_time(false); */
        utility::LogInfo("File {} opened", filename);
    } catch (const rs2::error &) {
        utility::LogWarning("Unable to open file {}", filename);
        return false;
    }
    filename_ = filename;
    is_eof_ = false;
    is_opened_ = true;
    metadata_.ConvertFromJsonValue(GetMetadataJson());
    // Launch thread to keep frame_buffer full
    frame_reader_thread_ =
            std::thread{&RSBagReader::fill_frame_buffer, this, start_time_us};
    return true;
}

void RSBagReader::Close() {
    filename_.clear();
    is_opened_ = false;
    need_frames_.notify_one();
    frame_reader_thread_.join();
    pipe_->stop();
}

Json::Value RSBagReader::GetMetadataJson() {
    using namespace std::literals::string_literals;
    // Names for common RealSense pixel formats. See
    // https://intelrealsense.github.io/librealsense/doxygen/rs__sensor_8h.html#ae04b7887ce35d16dbd9d2d295d23aac7
    // for format documentation
    static const std::unordered_map<rs2_format, std::string> format_name = {
            {RS2_FORMAT_Z16, "Z16"s},     {RS2_FORMAT_YUYV, "YUYV"s},
            {RS2_FORMAT_RGB8, "RGB8"s},   {RS2_FORMAT_BGR8, "BGR8"s},
            {RS2_FORMAT_RGBA8, "RGBA8"s}, {RS2_FORMAT_BGRA8, "BGRA8"s},
            {RS2_FORMAT_Y8, "Y8"s},       {RS2_FORMAT_Y16, "Y16"s}};
    static const std::unordered_map<rs2_format, core::Dtype> format_dtype = {
            {RS2_FORMAT_Z16, core::Dtype::UInt16},
            {RS2_FORMAT_YUYV, core::Dtype::UInt8},
            {RS2_FORMAT_RGB8, core::Dtype::UInt8},
            {RS2_FORMAT_BGR8, core::Dtype::UInt8},
            {RS2_FORMAT_RGBA8, core::Dtype::UInt8},
            {RS2_FORMAT_BGRA8, core::Dtype::UInt8},
            {RS2_FORMAT_Y8, core::Dtype::UInt8},
            {RS2_FORMAT_Y16, core::Dtype::UInt16}};
    static const std::unordered_map<rs2_format, uint8_t> format_channels = {
            {RS2_FORMAT_Z16, 1},  {RS2_FORMAT_YUYV, 2},  {RS2_FORMAT_RGB8, 3},
            {RS2_FORMAT_BGR8, 3}, {RS2_FORMAT_RGBA8, 4}, {RS2_FORMAT_BGRA8, 4},
            {RS2_FORMAT_Y8, 1},   {RS2_FORMAT_Y16, 1}};

    if (!IsOpened()) {
        utility::LogError("Null file handler. Please call Open().");
    }

    Json::Value value;

    const auto profile = pipe_->get_active_profile();
    const auto rs_device = profile.get_device().as<rs2::playback>();
    const auto rs_depth = profile.get_stream(RS2_STREAM_DEPTH)
                                  .as<rs2::video_stream_profile>();
    const auto rs_color = profile.get_stream(RS2_STREAM_COLOR)
                                  .as<rs2::video_stream_profile>();

    rs2_intrinsics rgb_intr = rs_color.get_intrinsics();
    camera::PinholeCameraIntrinsic pinhole_camera;
    pinhole_camera.SetIntrinsics(rgb_intr.width, rgb_intr.height, rgb_intr.fx,
                                 rgb_intr.fy, rgb_intr.ppx, rgb_intr.ppy);
    // TODO: Add support for distortion
    pinhole_camera.ConvertToJsonValue(value);

    value["device_name"] = rs_device.get_info(RS2_CAMERA_INFO_NAME);
    value["serial_number"] = rs_device.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER);
    value["depth_format"] = format_name.at(rs_depth.format());
    dt_depth_ = format_dtype.at(rs_depth.format());
    if (dt_depth_ != core::Dtype::UInt16) {
        utility::LogError("Only 16 bit unsigned int depth is supported!");
    }
    value["color_format"] = format_name.at(rs_color.format());
    dt_color_ = format_dtype.at(rs_color.format());
    if (dt_color_ != core::Dtype::UInt8) {
        utility::LogError("Only 8 bit unsigned int color is supported!");
    }
    channels_color_ = format_channels.at(rs_color.format());
    value["fps"] = rs_color.fps();
    if (value["fps"] != rs_depth.fps()) {
        utility::LogError(
                "Different frame rates for color ({} fps) and depth ({} fps) "
                "streams is not supported.",
                value["fps"], rs_depth.fps());
    }
    value["stream_length_usec"] =
            std::chrono::duration_cast<std::chrono::microseconds>(
                    rs_device.get_duration())
                    .count();

    return value;
}

void RSBagReader::fill_frame_buffer(uint64_t start_time_us) try {
    std::mutex frame_buffer_mutex;
    std::unique_lock<std::mutex> lock(frame_buffer_mutex);
    const unsigned int RS2_PLAYBACK_TIMEOUT_MS =
            (unsigned int)(10 * 1000.0 / metadata_.fps_);
    rs2::frameset frames;
    rs2::playback rs_device =
            pipe_->get_active_profile().get_device().as<rs2::playback>();
    rs_device.seek(std::chrono::microseconds(start_time_us));
    uint64_t next_dev_color_fid = 0;
    head_fid_ = 0;
    tail_fid_ = 0;  // do not write to tail_fid_ in this thread
    uint64_t dev_color_fid = 0;
    uint64_t nreq = 0, fid = 0;  // debug

    while (is_opened_) {
        rs_device.resume();
        utility::LogDebug(
                "frame_reader_thread_ start reading tail_fid_={}, head_fid_={}",
                tail_fid_, head_fid_);
        while (!is_eof_ && head_fid_ < tail_fid_ + frame_buffer_.size()) {
            // Ensure next frameset is not a repeat
            while (next_dev_color_fid <= dev_color_fid &&
                   pipe_->try_wait_for_frames(&frames,
                                              RS2_PLAYBACK_TIMEOUT_MS)) {
                ++nreq;
                next_dev_color_fid =
                        frames.get_color_frame().get_frame_number();
            }
            ++fid;
            if (next_dev_color_fid > dev_color_fid) {
                dev_color_fid = next_dev_color_fid;
                auto &current_frame =
                        frame_buffer_[head_fid_ % frame_buffer_.size()];

                frames = align_to_color_->process(frames);
                const auto &color_frame = frames.get_color_frame();
                // Copy frame data to Tensors
                current_frame.color_ = core::Tensor(
                        static_cast<const uint8_t *>(color_frame.get_data()),
                        {color_frame.get_height(), color_frame.get_width(),
                         channels_color_},
                        dt_color_);
                const auto &depth_frame = frames.get_depth_frame();
                current_frame.depth_ = core::Tensor(
                        static_cast<const uint16_t *>(depth_frame.get_data()),
                        {depth_frame.get_height(), depth_frame.get_width()},
                        dt_depth_);
                frame_position_us_[head_fid_ % frame_buffer_.size()] =
                        rs_device.get_position() /
                        1000;  // Convert nanoseconds -> microseconds
                ++head_fid_;   // atomic
                utility::LogDebug(
                        "Device Frame {}, Request {}, output frame {}",
                        dev_color_fid, nreq, fid);
            } else {
                utility::LogDebug("frame_reader_thread_ EOF reached");
                is_eof_ = true;
                return;
            }
            if (!is_opened_) break;  // exit if SeekTimestamp() / Close()
        }
        rs_device.pause();  // Pause playback to prevent frame drops
        utility::LogDebug(
                "frame_reader_thread_ pause reading tail_fid_={}, head_fid_={}",
                tail_fid_, head_fid_);
        need_frames_.wait(lock, [this] {
            return !is_opened_ ||
                   head_fid_ < tail_fid_ + frame_buffer_.size() /
                                                   BUFFER_REFILL_FACTOR;
        });
    }
} catch (const rs2::error &e) {
    utility::LogError("Realsense function call {}({}) error.",
                      e.get_failed_function(), e.get_failed_args());
} catch (const std::exception &e) {
    utility::LogError("Error in reading RealSense bag file: {}", e.what());
}

bool RSBagReader::IsEOF() const { return is_eof_ && tail_fid_ == head_fid_; }

t::geometry::RGBDImage RSBagReader::NextFrame() {
    if (!IsOpened()) {
        utility::LogError("Null file handler. Please call Open().");
    }
    if (!is_eof_ &&
        head_fid_ < tail_fid_ + frame_buffer_.size() / BUFFER_REFILL_FACTOR)
        need_frames_.notify_one();

    while (!is_eof_ &&
           tail_fid_ ==
                   head_fid_) {  // (rare) spin wait for frame_reader_thread_
        std::this_thread::sleep_for(
                std::chrono::duration<double>(1 / metadata_.fps_));
    }
    if (is_eof_ && tail_fid_ == head_fid_) {  // no more frames
        utility::LogInfo("EOF reached");
        return t::geometry::RGBDImage();
    } else
        return frame_buffer_[(tail_fid_++) %  // atomic
                             frame_buffer_.size()];
}

bool RSBagReader::SeekTimestamp(uint64_t timestamp) {
    if (!IsOpened()) {
        utility::LogWarning("Null file handler. Please call Open().");
        return false;
    }

    if (timestamp >= metadata_.stream_length_usec_) {
        utility::LogWarning("Timestamp {} exceeds maximum {} (us).", timestamp,
                            metadata_.stream_length_usec_);
        return false;
    }

    auto rs_device =
            pipe_->get_active_profile().get_device().as<rs2::playback>();
    Open(rs_device.file_name(), timestamp);  // restart streaming
    return true;
}

uint64_t RSBagReader::GetTimestamp() const {
    if (!IsOpened()) {
        utility::LogWarning("Null file handler. Please call Open().");
        return UINT64_MAX;
    }
    return tail_fid_ == 0
                   ? 0
                   : frame_position_us_[(tail_fid_ - 1) % frame_buffer_.size()];
}

}  // namespace io
}  // namespace t
}  // namespace open3d
