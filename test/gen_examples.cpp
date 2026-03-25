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

// Generates cropped thumbnail examples from JPEG testdata.
//
// For each input image, produces two output files:
//   <stem>_smart_crop.jpg  — square crop biased to 60% vertical centre
//   <stem>_detect_crop.jpg — crop around detected object bbox (or smart_crop
//                            fallback when no object is found or no model given)
//
// Usage:
//   bazel run //test:gen_examples -- <out_dir> <image.jpg> [<image.jpg> ...]
//
// With object detection model (optional):
//   ONVIF_MODEL_DIR=/path/to/models bazel run //test:gen_examples -- <out_dir> ...

#include <stddef.h>   // for size_t (needed before jpeglib.h)
#include <stdio.h>    // for FILE (needed before jpeglib.h)
#include <jpeglib.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "../jpeg_crop.hpp"
#include "../object_detect.hpp"

ABSL_FLAG(std::string, model_dir, "",
    "Directory containing nanodet_m.param and nanodet_m.bin; "
    "if empty, smart-crop heuristic is used.");

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto sz = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  f.read(reinterpret_cast<char*>(buf.data()), sz);
  return buf;
}

bool write_file(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
  return f.good();
}

// Read image dimensions from JPEG header without full decompress.
bool jpeg_dimensions(const std::vector<uint8_t>& jpeg, int* w, int* h) {
  struct JpegErr {
    jpeg_error_mgr base;
    bool fatal = false;
  } err;
  jpeg_decompress_struct info{};
  info.err = jpeg_std_error(&err.base);
  err.base.error_exit = [](j_common_ptr c) {
    reinterpret_cast<JpegErr*>(c->err)->fatal = true;
  };
  err.base.output_message = [](j_common_ptr) {};
  jpeg_create_decompress(&info);
  jpeg_mem_src(&info,
               const_cast<unsigned char*>(
                   reinterpret_cast<const unsigned char*>(jpeg.data())),
               static_cast<unsigned long>(jpeg.size()));  // NOLINT(runtime/int)
  jpeg_read_header(&info, TRUE);
  *w = static_cast<int>(info.image_width);
  *h = static_cast<int>(info.image_height);
  jpeg_destroy_decompress(&info);
  return !err.fatal && *w > 0 && *h > 0;
}

// Stem of a file path: "/a/b/foo.jpg" → "foo".
std::string stem(const std::string& path) {
  auto slash = path.rfind('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  auto dot = base.rfind('.');
  return (dot == std::string::npos) ? base : base.substr(0, dot);
}

}  // namespace

int main(int argc, char* argv[]) {
  std::vector<char*> args = absl::ParseCommandLine(argc, argv);
  if (args.size() < 3) {
    std::cerr << "Usage: " << args[0]
              << " <out_dir> <image.jpg> [<image.jpg> ...]\n"
              << "  --model_dir=<dir>  optional: load NanoDet-M from dir\n";
    return 1;
  }

  const std::string out_dir = args[1];
  // Create output directory (equivalent to mkdir -p).
  std::string mkdir_cmd = "mkdir -p " + out_dir;
  if (std::system(mkdir_cmd.c_str()) != 0) {  // NOLINT(runtime/threadsafe_fn)
    std::cerr << "Cannot create output directory: " << out_dir << "\n";
    return 1;
  }

  // Load object detector if model dir is set.
  std::unique_ptr<object_detect::ObjectDetector> detector;
  const std::string model_dir = absl::GetFlag(FLAGS_model_dir);
  if (!model_dir.empty()) {
    std::string param = model_dir + "/nanodet_m.param";
    std::string bin   = model_dir + "/nanodet_m.bin";
    auto result = object_detect::ObjectDetector::Load(param, bin);
    if (result.ok()) {
      detector = std::move(*result);
      std::cout << "Loaded NanoDet-M from " << model_dir << "\n";
    } else {
      std::cerr << "Warning: could not load model: "
                << result.status().message() << "\n";
    }
  }

  int ok = 0, fail = 0;
  for (size_t i = 2; i < args.size(); ++i) {
    const std::string in_path = args[i];
    const auto input = read_file(in_path);
    if (input.empty()) {
      std::cerr << "  skip " << in_path << ": cannot read\n";
      ++fail;
      continue;
    }

    int w = 0, h = 0;
    if (!jpeg_dimensions(input, &w, &h)) {
      std::cerr << "  skip " << in_path << ": bad JPEG header\n";
      ++fail;
      continue;
    }

    // --- smart crop ---
    auto smart = jpeg_crop::smart_crop(input);
    if (smart.empty()) {
      std::cerr << "  skip " << in_path << ": smart_crop failed\n";
      ++fail;
      continue;
    }
    std::string smart_out = out_dir + "/" + stem(in_path) + "_smart_crop.jpg";
    if (!write_file(smart_out, smart)) {
      std::cerr << "  error writing " << smart_out << "\n";
      ++fail;
      continue;
    }

    // --- detect crop (or smart_crop fallback) ---
    std::optional<jpeg_crop::BoundingBox> det_bbox;
    if (detector) det_bbox = detector->detect(input);

    jpeg_crop::BoundingBox box =
        jpeg_crop::select_crop_box(w, h, nullptr,
                                   det_bbox ? &*det_bbox : nullptr);
    auto detect = jpeg_crop::crop(input, box);
    if (detect.empty()) {
      std::cerr << "  skip " << in_path << ": detect_crop failed\n";
      ++fail;
      continue;
    }
    const char* kind = det_bbox ? "detect" : "smart";
    std::string detect_out =
        out_dir + "/" + stem(in_path) + "_" + kind + "_crop.jpg";
    if (!write_file(detect_out, detect)) {
      std::cerr << "  error writing " << detect_out << "\n";
      ++fail;
      continue;
    }

    std::cout << stem(in_path)
              << "  " << w << "×" << h
              << "  →  smart_crop: " << smart.size() / 1024 << " KB"
              << "  |  " << kind << "_crop: " << detect.size() / 1024 << " KB";
    if (det_bbox)
      std::printf("  bbox=(%.3f,%.3f,%.3f,%.3f)",
                  det_bbox->x, det_bbox->y, det_bbox->w, det_bbox->h);
    std::cout << "\n";
    ++ok;
  }

  std::cout << "\n" << ok << " image(s) written to " << out_dir << "/\n";
  if (fail) std::cout << fail << " image(s) failed.\n";
  return fail ? 1 : 0;
}
