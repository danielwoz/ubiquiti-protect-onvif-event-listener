workspace(name = "onvif")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

# Abseil C++ — provides absl::Status / absl::StatusOr.
http_archive(
    name = "com_google_absl",
    sha256 = "f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3",
    strip_prefix = "abseil-cpp-20240722.0",
    urls = ["https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz"],
)

load("//bazel:pkg_config.bzl", "pkg_config_library")
load("//bazel:arm64_sysroot.bzl", "arm64_sysroot")

# Host (x86-64) system libraries via pkg-config.
# extra_linkopts supplies transitive static deps that pkg-config --static misses.
pkg_config_library(name = "libxml2", pkg = "libxml-2.0",
    extra_linkopts = ["-licui18n", "-licuuc", "-licudata", "-llzma", "-lz"])
pkg_config_library(name = "libcurl", pkg = "libcurl",
    extra_linkopts = ["-lunistring", "-lrtmp", "-lnettle", "-lhogweed", "-lgmp",
                      "-lgnutls", "-ltasn1", "-lp11-kit",
                      "-lgcrypt", "-lgpg-error",
                      "-lsasl2", "-lbrotlicommon",
                      "-lgssapi_krb5", "-lssl", "-lcrypto",
                      "-Wl,--allow-multiple-definition"])
pkg_config_library(name = "openssl",       pkg = "openssl")
pkg_config_library(name = "libmicrohttpd", pkg = "libmicrohttpd",
    extra_linkopts = ["-lgnutls", "-lhogweed", "-lnettle", "-lgmp",
                      "-ltasn1", "-lunistring", "-lp11-kit"])
pkg_config_library(name = "libpq", pkg = "libpq",
    extra_linkopts = ["-lpgcommon", "-lpgport", "-lgssapi_krb5", "-lssl", "-lcrypto"])
pkg_config_library(name = "libjpeg", pkg = "libjpeg")

# NCNN — lightweight neural network inference for on-device object detection.
http_archive(
    name = "ncnn",
    sha256 = "2fdc5c6e37f8552921a9daad498a1be54a6fa6edd32c1a9e3030b27fab253b47",
    strip_prefix = "ncnn-20260113",
    url = "https://github.com/Tencent/ncnn/archive/refs/tags/20260113.tar.gz",
    build_file = "//third_party:ncnn.BUILD",
)

# NanoDet-M model files for object detection.
http_file(
    name = "nanodet_m_param",
    sha256 = "8543dccd5604ded10d06bdb2b2f702f8f2f1dac09652c81750f21bf0a6e3f1a8",
    urls = ["https://github.com/nihui/ncnn-assets/raw/refs/heads/master/models/nanodet_m.param"],
)
http_file(
    name = "nanodet_m_bin",
    sha256 = "8d7f846cfc340a3ef66389f174a66819709f7182b9d35788ee1506679caac65e",
    urls = ["https://github.com/nihui/ncnn-assets/raw/refs/heads/master/models/nanodet_m.bin"],
)

# arm64 cross-compilation sysroot + toolchain.
# Declares the repository; packages are downloaded only when --config=arm64
# is used (via --extra_toolchains in .bazelrc), not on every host build.
arm64_sysroot(name = "arm64_sysroot")
