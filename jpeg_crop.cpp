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

#include "jpeg_crop.hpp"

#include <stddef.h>   // jpeglib.h needs size_t
#include <stdio.h>    // jpeglib.h needs FILE
#include <jpeglib.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace jpeg_crop {

namespace {

// Custom libjpeg error handler that avoids calling exit() on errors.
struct JpegErrorMgr {
  jpeg_error_mgr base;   // must be first
  bool           fatal;
};

void jpeg_error_exit(j_common_ptr cinfo) {
  auto* mgr   = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
  mgr->fatal  = true;
  // Suppress default message output.
  // Execution continues; callers must check mgr->fatal after each libjpeg call.
}

void jpeg_output_message(j_common_ptr /*cinfo*/) {}  // silence all output

}  // namespace

// ---------------------------------------------------------------------------
// smart_crop_box
// ---------------------------------------------------------------------------

BoundingBox smart_crop_box(int width, int height) {
  BoundingBox box;
  // Square whose side equals the shorter dimension.
  const float side = static_cast<float>(std::min(width, height));
  box.w = side / static_cast<float>(width);
  box.h = side / static_cast<float>(height);
  // Centre horizontally.
  box.x = (1.0f - box.w) / 2.0f;
  // Bias vertically: place the centre at 60 % from the top.  Security cameras
  // are mounted high; subjects occupy the lower portion of the frame.
  // Centre at 0.60 → top of square at (0.60 − box.h/2).
  box.y = 0.60f - box.h / 2.0f;
  // Clamp to [0, 1].
  box.y = std::max(0.0f, std::min(box.y, 1.0f - box.h));
  return box;
}

// ---------------------------------------------------------------------------
// crop
// ---------------------------------------------------------------------------

std::vector<uint8_t> crop(const std::vector<uint8_t>& input_jpeg,
                           const BoundingBox& box) {
  if (input_jpeg.empty()) return {};

  // ---- Decompress --------------------------------------------------------
  jpeg_decompress_struct srcinfo{};
  JpegErrorMgr           srcerr{};
  srcerr.fatal    = false;
  srcinfo.err     = jpeg_std_error(&srcerr.base);
  srcerr.base.error_exit     = jpeg_error_exit;
  srcerr.base.output_message = jpeg_output_message;
  jpeg_create_decompress(&srcinfo);

  jpeg_mem_src(&srcinfo,
               // libjpeg takes unsigned char*, so cast from uint8_t*
               const_cast<unsigned char*>(
                   reinterpret_cast<const unsigned char*>(input_jpeg.data())),
               static_cast<unsigned long>(input_jpeg.size()));  // NOLINT(runtime/int)

  jpeg_read_header(&srcinfo, TRUE);
  if (srcerr.fatal) {
    jpeg_destroy_decompress(&srcinfo);
    return {};
  }

  srcinfo.out_color_space = JCS_RGB;
  jpeg_start_decompress(&srcinfo);
  if (srcerr.fatal) {
    jpeg_destroy_decompress(&srcinfo);
    return {};
  }

  const int src_w = static_cast<int>(srcinfo.output_width);
  const int src_h = static_cast<int>(srcinfo.output_height);
  const int channels = 3;  // JCS_RGB

  // Compute pixel-space crop rectangle from normalised box.
  int cx = static_cast<int>(box.x * src_w);
  int cy = static_cast<int>(box.y * src_h);
  int cw = static_cast<int>(box.w * src_w);
  int ch = static_cast<int>(box.h * src_h);

  // Clamp to image bounds.
  cx = std::max(0, std::min(cx, src_w - 1));
  cy = std::max(0, std::min(cy, src_h - 1));
  cw = std::max(1, std::min(cw, src_w - cx));
  ch = std::max(1, std::min(ch, src_h - cy));

  // Allocate output pixel buffer.
  std::vector<uint8_t> cropped(static_cast<size_t>(cw) * ch * channels);

  // Read scanlines, keeping only those within [cy, cy+ch).
  const size_t row_stride = static_cast<size_t>(src_w) * channels;
  std::vector<uint8_t> row_buf(row_stride);
  unsigned char* row_ptr = row_buf.data();

  int out_row = 0;
  while (static_cast<int>(srcinfo.output_scanline) < src_h && !srcerr.fatal) {
    jpeg_read_scanlines(&srcinfo, &row_ptr, 1);
    const int y = static_cast<int>(srcinfo.output_scanline) - 1;
    if (y >= cy && y < cy + ch) {
      const size_t src_off = static_cast<size_t>(cx) * channels;
      const size_t dst_off = static_cast<size_t>(out_row) * cw * channels;
      std::memcpy(cropped.data() + dst_off,
                  row_buf.data() + src_off,
                  static_cast<size_t>(cw) * channels);
      ++out_row;
    }
  }

  jpeg_finish_decompress(&srcinfo);
  jpeg_destroy_decompress(&srcinfo);

  if (srcerr.fatal || out_row == 0) return {};

  // ---- Re-encode ---------------------------------------------------------
  jpeg_compress_struct dstinfo{};
  JpegErrorMgr         dsterr{};
  dsterr.fatal    = false;
  dstinfo.err     = jpeg_std_error(&dsterr.base);
  dsterr.base.error_exit     = jpeg_error_exit;
  dsterr.base.output_message = jpeg_output_message;
  jpeg_create_compress(&dstinfo);

  unsigned char* outbuf  = nullptr;
  unsigned long  outsize = 0;  // NOLINT(runtime/int)
  jpeg_mem_dest(&dstinfo, &outbuf, &outsize);

  dstinfo.image_width      = static_cast<JDIMENSION>(cw);
  dstinfo.image_height     = static_cast<JDIMENSION>(ch);
  dstinfo.input_components = channels;
  dstinfo.in_color_space   = JCS_RGB;
  jpeg_set_defaults(&dstinfo);
  jpeg_set_quality(&dstinfo, kOutputQuality, TRUE);

  jpeg_start_compress(&dstinfo, TRUE);
  if (dsterr.fatal) {
    jpeg_destroy_compress(&dstinfo);
    if (outbuf) free(outbuf);  // NOLINT(cppcoreguidelines-no-malloc)
    return {};
  }

  for (int r = 0; r < ch && !dsterr.fatal; ++r) {
    unsigned char* scanline =
        cropped.data() + static_cast<size_t>(r) * cw * channels;
    jpeg_write_scanlines(&dstinfo, &scanline, 1);
  }

  jpeg_finish_compress(&dstinfo);
  jpeg_destroy_compress(&dstinfo);

  if (dsterr.fatal || outbuf == nullptr) {
    if (outbuf) free(outbuf);  // NOLINT(cppcoreguidelines-no-malloc)
    return {};
  }

  std::vector<uint8_t> result(outbuf, outbuf + outsize);
  free(outbuf);  // NOLINT(cppcoreguidelines-no-malloc)
  return result;
}

// ---------------------------------------------------------------------------
// smart_crop
// ---------------------------------------------------------------------------

std::vector<uint8_t> smart_crop(const std::vector<uint8_t>& input_jpeg) {
  if (input_jpeg.empty()) return {};

  // Read dimensions only (no full decompress needed).
  jpeg_decompress_struct info{};
  JpegErrorMgr           err{};
  err.fatal    = false;
  info.err     = jpeg_std_error(&err.base);
  err.base.error_exit     = jpeg_error_exit;
  err.base.output_message = jpeg_output_message;
  jpeg_create_decompress(&info);

  jpeg_mem_src(&info,
               const_cast<unsigned char*>(
                   reinterpret_cast<const unsigned char*>(input_jpeg.data())),
               static_cast<unsigned long>(input_jpeg.size()));  // NOLINT(runtime/int)

  jpeg_read_header(&info, TRUE);
  const int w = static_cast<int>(info.image_width);
  const int h = static_cast<int>(info.image_height);
  jpeg_destroy_decompress(&info);

  if (err.fatal || w == 0 || h == 0) return {};

  const BoundingBox box = smart_crop_box(w, h);
  return crop(input_jpeg, box);
}

// ---------------------------------------------------------------------------
// select_crop_box
// ---------------------------------------------------------------------------

BoundingBox select_crop_box(int width, int height,
                             const BoundingBox* onvif_bbox,
                             const BoundingBox* det_bbox) {
  const float pad = kBboxPadding *
      static_cast<float>(std::min(width, height)) /
      static_cast<float>(std::max(width, height));

  auto pad_box = [&](const BoundingBox& b) -> BoundingBox {
    BoundingBox r;
    r.x = std::max(0.0f, b.x - pad);
    r.y = std::max(0.0f, b.y - pad);
    r.w = std::min(1.0f - r.x, b.w + 2.0f * pad);
    r.h = std::min(1.0f - r.y, b.h + 2.0f * pad);
    return r;
  };

  if (onvif_bbox && onvif_bbox->w > 0.0f && onvif_bbox->h > 0.0f)
    return pad_box(*onvif_bbox);
  if (det_bbox && det_bbox->w > 0.0f && det_bbox->h > 0.0f)
    return pad_box(*det_bbox);
  return smart_crop_box(width, height);
}

}  // namespace jpeg_crop
