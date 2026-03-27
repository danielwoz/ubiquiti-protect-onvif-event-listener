# Dependencies

All C libraries are built from source by Bazel — no library dev packages are
required from apt. Only build tools need to be installed on the host.

## Git submodules

Pinned to a specific commit; update with `git submodule update --remote`.

| Submodule | Used for |
|-----------|----------|
| `third_party/libjpeg-turbo` | JPEG decode, crop, and re-encode for detection thumbnails (`jpeg_crop.cpp`) |
| `third_party/libxml2` | Parsing ONVIF SOAP/XML responses from cameras (`onvif_listener.cpp`) |
| `third_party/openssl` | TLS for HTTPS connections to cameras (libcurl backend) |
| `third_party/curl` | HTTP/HTTPS client for ONVIF WS-PullPoint subscriptions (`onvif_listener.cpp`) |

## Bazel HTTP archives

Downloaded and SHA-verified by Bazel on first use; cached in the Bazel output base.

| Archive | Used for |
|---------|----------|
| `abseil-cpp` | `absl::Status`, `absl::StatusOr`, `absl::flags`, `absl::log` — used throughout |
| `postgresql` (libpq) | C client API for the UniFi Protect PostgreSQL database (`detection_recorder.cpp`, `unifi_camera_config.cpp`) |
| `libmicrohttpd` | Embedded HTTP server used by the fake camera in tests only; not in the production binary |
| `ncnn` | Neural network inference engine for on-device NanoDet-M object detection (`object_detect.cpp`) |
| `gmp` | Arbitrary-precision arithmetic — transitive dependency of Nettle |
| `nettle` / `hogweed` | Symmetric and public-key crypto — transitive dependency of libmicrohttpd |
| `libtasn1` | ASN.1 parser — transitive dependency of Nettle |

## apt packages (build tools only)

| Package | Used for |
|---------|----------|
| `build-essential` | gcc, make, ar — required to build vendored C libraries via autoconf/cmake |
| `clang` | C++ compiler for the main binary and tests (`--config=clang`) |
| `cmake` | Build system for libjpeg-turbo, libxml2, libcurl, ncnn |
| `ninja-build` | Fast build backend used by cmake |
| `perl` | Required by OpenSSL's `Configure` script |
| `python3-cpplint` | Linter for the pre-push checklist (`python3 -m cpplint`) |
