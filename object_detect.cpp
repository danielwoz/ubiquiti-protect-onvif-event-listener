// Copyright 2026 Daniel W
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "object_detect.hpp"

#include <stddef.h>  // size_t for jpeglib
#include <stdio.h>   // FILE for jpeglib
#include <jpeglib.h>

#ifdef WITH_NCNN
#include <ncnn/net.h>
#endif  // WITH_NCNN

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "jpeg_crop.hpp"

namespace object_detect {

// ---------------------------------------------------------------------------
// Impl struct (holds the NCNN net when compiled in)
// ---------------------------------------------------------------------------

struct ObjectDetector::Impl {
#ifdef WITH_NCNN
  ncnn::Net net;
  int input_w = 320;
  int input_h = 320;
#endif  // WITH_NCNN
};

// ---------------------------------------------------------------------------
// NCNN-only helpers
// ---------------------------------------------------------------------------

#ifdef WITH_NCNN

namespace {

// Detection result before NMS.
struct Det {
  float x, y, w, h;  // in padded-input pixel space
  float conf;
  int   cls;
};

// Fast approximate exp for softmax.
static inline float fast_exp(float x) {
  union { uint32_t i; float f; } v;  // NOLINT(runtime/int)
  v.i = static_cast<uint32_t>(
      static_cast<int32_t>(1.4426950409f * x + 126.93490512f) << 23);
  return v.f;
}

static void softmax_inplace(float* data, int n) {
  float sum = 0.0f;
  for (int i = 0; i < n; ++i) {
    data[i] = fast_exp(data[i]);
    sum += data[i];
  }
  for (int i = 0; i < n; ++i) data[i] /= sum;
}

// reg_max_1 = 8 for nanodet_m (reg_max=7, so reg_max_1 = 7+1 = 8).
static constexpr int kRegMaxP1 = 8;

static void generate_proposals(const ncnn::Mat& cls_pred,
                                const ncnn::Mat& dis_pred,
                                int stride,
                                int pad_w, int pad_h,
                                float prob_threshold,
                                std::vector<Det>& dets) {
  const int num_grid_x = pad_w / stride;
  const int num_grid_y = pad_h / stride;
  const int num_class  = cls_pred.w;

  for (int i = 0; i < num_grid_y; ++i) {
    for (int j = 0; j < num_grid_x; ++j) {
      const int idx = i * num_grid_x + j;
      const float* scores = cls_pred.row(idx);

      int   label = -1;
      float score = -1.0e9f;
      for (int k = 0; k < num_class; ++k) {
        if (scores[k] > score) {
          label = k;
          score = scores[k];
        }
      }
      if (score < prob_threshold) continue;

      // Decode distance predictions with softmax + weighted sum.
      float ltrb[4] = {};
      for (int k = 0; k < 4; ++k) {
        float buf[kRegMaxP1];
        const float* row_ptr = dis_pred.row(idx);
        for (int r = 0; r < kRegMaxP1; ++r)
          buf[r] = row_ptr[k * kRegMaxP1 + r];
        softmax_inplace(buf, kRegMaxP1);
        float dis = 0.0f;
        for (int r = 0; r < kRegMaxP1; ++r)
          dis += buf[r] * static_cast<float>(r);
        ltrb[k] = dis * static_cast<float>(stride);
      }

      const float pb_cx = (static_cast<float>(j) + 0.5f) *
                          static_cast<float>(stride);
      const float pb_cy = (static_cast<float>(i) + 0.5f) *
                          static_cast<float>(stride);

      const float x0 = pb_cx - ltrb[0];
      const float y0 = pb_cy - ltrb[1];
      const float x1 = pb_cx + ltrb[2];
      const float y1 = pb_cy + ltrb[3];

      dets.push_back({x0, y0, x1 - x0, y1 - y0, score, label});
    }
  }
}

static float iou(const Det& a, const Det& b) {
  const float ax1 = a.x,            ay1 = a.y;
  const float ax2 = a.x + a.w,      ay2 = a.y + a.h;
  const float bx1 = b.x,            by1 = b.y;
  const float bx2 = b.x + b.w,      by2 = b.y + b.h;

  const float inter_w = std::max(0.0f, std::min(ax2, bx2) - std::max(ax1, bx1));
  const float inter_h = std::max(0.0f, std::min(ay2, by2) - std::max(ay1, by1));
  const float inter   = inter_w * inter_h;
  const float uni     = a.w * a.h + b.w * b.h - inter;
  return uni > 0.0f ? inter / uni : 0.0f;
}

static void nms_inplace(std::vector<Det>& dets, float iou_threshold) {
  std::sort(dets.begin(), dets.end(),
            [](const Det& a, const Det& b) { return a.conf > b.conf; });
  std::vector<bool> suppressed(dets.size(), false);
  for (size_t i = 0; i < dets.size(); ++i) {
    if (suppressed[i]) continue;
    for (size_t j = i + 1; j < dets.size(); ++j) {
      if (!suppressed[j] && iou(dets[i], dets[j]) > iou_threshold)
        suppressed[j] = true;
    }
  }
  std::vector<Det> out;
  out.reserve(dets.size());
  for (size_t i = 0; i < dets.size(); ++i) {
    if (!suppressed[i]) out.push_back(dets[i]);
  }
  dets = std::move(out);
}

// Libjpeg error handler that avoids exit() on errors.
struct JpegErr {
  jpeg_error_mgr base;  // must be first
  bool           fatal;
};
static void jpeg_err_exit(j_common_ptr cinfo) {
  reinterpret_cast<JpegErr*>(cinfo->err)->fatal = true;
}
static void jpeg_out_msg(j_common_ptr /*cinfo*/) {}

}  // namespace

#endif  // WITH_NCNN

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

absl::StatusOr<std::unique_ptr<ObjectDetector>> ObjectDetector::Load(
    const std::string& param_path, const std::string& bin_path) {
#ifndef WITH_NCNN
  (void)param_path;
  (void)bin_path;
  return absl::UnimplementedError(
      "object_detect: compiled without NCNN support");
#else
  auto obj = std::unique_ptr<ObjectDetector>(new ObjectDetector());
  obj->impl_ = std::make_unique<Impl>();

  obj->impl_->net.opt.use_vulkan_compute = false;

  if (obj->impl_->net.load_param(param_path.c_str()) != 0)
    return absl::NotFoundError("object_detect: cannot load param: " +
                               param_path);
  if (obj->impl_->net.load_model(bin_path.c_str()) != 0)
    return absl::NotFoundError("object_detect: cannot load bin: " + bin_path);

  return obj;
#endif  // WITH_NCNN
}

// ---------------------------------------------------------------------------
// Destructor
// ---------------------------------------------------------------------------

ObjectDetector::~ObjectDetector() = default;

// ---------------------------------------------------------------------------
// detect
// ---------------------------------------------------------------------------

std::optional<Detection> ObjectDetector::detect(
    const std::vector<uint8_t>& jpeg_bytes) const {
#ifndef WITH_NCNN
  (void)jpeg_bytes;
  return std::nullopt;
#else
  if (!impl_ || jpeg_bytes.empty()) return std::nullopt;

  // ---- Decode JPEG to RGB pixels -----------------------------------------
  jpeg_decompress_struct srcinfo{};
  JpegErr                srcerr{};
  srcerr.fatal                   = false;
  srcinfo.err                    = jpeg_std_error(&srcerr.base);
  srcerr.base.error_exit         = jpeg_err_exit;
  srcerr.base.output_message     = jpeg_out_msg;
  jpeg_create_decompress(&srcinfo);

  jpeg_mem_src(&srcinfo,
               const_cast<unsigned char*>(
                   reinterpret_cast<const unsigned char*>(jpeg_bytes.data())),
               static_cast<unsigned long>(jpeg_bytes.size()));  // NOLINT(runtime/int)
  jpeg_read_header(&srcinfo, TRUE);
  if (srcerr.fatal) {
    jpeg_destroy_decompress(&srcinfo);
    return std::nullopt;
  }

  srcinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&srcinfo);
  if (srcerr.fatal) {
    jpeg_destroy_decompress(&srcinfo);
    return std::nullopt;
  }

  const int orig_w    = static_cast<int>(srcinfo.output_width);
  const int orig_h    = static_cast<int>(srcinfo.output_height);
  const int channels  = 3;
  const size_t stride = static_cast<size_t>(orig_w) * channels;

  std::vector<uint8_t> pixels(stride * static_cast<size_t>(orig_h));
  std::vector<uint8_t> row_buf(stride);
  unsigned char* row_ptr = row_buf.data();

  while (static_cast<int>(srcinfo.output_scanline) < orig_h && !srcerr.fatal) {
    jpeg_read_scanlines(&srcinfo, &row_ptr, 1);
    const int y = static_cast<int>(srcinfo.output_scanline) - 1;
    std::memcpy(pixels.data() + static_cast<size_t>(y) * stride,
                row_buf.data(), stride);
  }
  jpeg_finish_decompress(&srcinfo);
  jpeg_destroy_decompress(&srcinfo);
  if (srcerr.fatal || orig_w == 0 || orig_h == 0) return std::nullopt;

  // ---- Compute letterbox padding -----------------------------------------
  const int target_w = impl_->input_w;
  const int target_h = impl_->input_h;

  const float scale = std::min(
      static_cast<float>(target_w) / static_cast<float>(orig_w),
      static_cast<float>(target_h) / static_cast<float>(orig_h));
  const int scaled_w = static_cast<int>(static_cast<float>(orig_w) * scale);
  const int scaled_h = static_cast<int>(static_cast<float>(orig_h) * scale);
  const int wpad     = target_w - scaled_w;
  const int hpad     = target_h - scaled_h;

  // ---- Resize + pad with NCNN letterbox -----------------------------------
  // Use PIXEL_RGB2BGR so nanodet_m gets BGR (trained on BGR/OpenCV images).
  ncnn::Mat in = ncnn::Mat::from_pixels_resize(
      pixels.data(), ncnn::Mat::PIXEL_RGB2BGR,
      orig_w, orig_h, scaled_w, scaled_h);

  ncnn::Mat in_pad;
  ncnn::copy_make_border(in, in_pad,
                         hpad / 2, hpad - hpad / 2,
                         wpad / 2, wpad - wpad / 2,
                         ncnn::BORDER_CONSTANT, 114.0f);

  // Normalise: subtract mean (BGR), divide by std.
  const float mean_vals[3] = {103.53f, 116.28f, 123.675f};
  const float norm_vals[3] = {0.017125f, 0.017507f, 0.017429f};
  in_pad.substract_mean_normalize(mean_vals, norm_vals);

  // ---- Run inference ------------------------------------------------------
  ncnn::Extractor ex = impl_->net.create_extractor();
  ex.input("input.1", in_pad);

  ncnn::Mat cls_stride8,  dis_stride8;
  ncnn::Mat cls_stride16, dis_stride16;
  ncnn::Mat cls_stride32, dis_stride32;

  ex.extract("792", cls_stride8);
  ex.extract("795", dis_stride8);
  ex.extract("814", cls_stride16);
  ex.extract("817", dis_stride16);
  ex.extract("836", cls_stride32);
  ex.extract("839", dis_stride32);

  // ---- Decode proposals ---------------------------------------------------
  static constexpr float kProbThreshold = 0.35f;
  static constexpr float kNmsThreshold  = 0.5f;

  std::vector<Det> dets;
  generate_proposals(cls_stride8,  dis_stride8,  8,
                     target_w, target_h, kProbThreshold, dets);
  generate_proposals(cls_stride16, dis_stride16, 16,
                     target_w, target_h, kProbThreshold, dets);
  generate_proposals(cls_stride32, dis_stride32, 32,
                     target_w, target_h, kProbThreshold, dets);

  nms_inplace(dets, kNmsThreshold);

  // ---- Filter to security-relevant classes --------------------------------
  Det* best = nullptr;
  for (auto& d : dets) {
    if (!is_security_relevant(d.cls)) continue;
    if (!best || d.conf > best->conf) best = &d;
  }
  if (!best) return std::nullopt;

  // ---- Scale box back to original image coords and normalise to [0,1] -----
  const float off_x = static_cast<float>(wpad / 2);
  const float off_y = static_cast<float>(hpad / 2);

  const float x_orig = (best->x - off_x) / scale;
  const float y_orig = (best->y - off_y) / scale;
  const float w_orig = best->w / scale;
  const float h_orig = best->h / scale;

  jpeg_crop::BoundingBox bb;
  bb.x = std::max(0.0f, std::min(1.0f, x_orig / static_cast<float>(orig_w)));
  bb.y = std::max(0.0f, std::min(1.0f, y_orig / static_cast<float>(orig_h)));
  bb.w = std::max(0.0f,
                  std::min(1.0f - bb.x,
                           w_orig / static_cast<float>(orig_w)));
  bb.h = std::max(0.0f,
                  std::min(1.0f - bb.y,
                           h_orig / static_cast<float>(orig_h)));

  if (bb.w <= 0.0f || bb.h <= 0.0f) return std::nullopt;
  return Detection{bb, best->cls};
#endif  // WITH_NCNN
}

}  // namespace object_detect
