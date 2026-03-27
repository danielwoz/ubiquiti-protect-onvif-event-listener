workspace(name = "onvif")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive", "http_file")

# Abseil C++ — provides absl::Status / absl::StatusOr.
http_archive(
    name = "com_google_absl",
    sha256 = "f50e5ac311a81382da7fa75b97310e4b9006474f9560ac46f54a9967f07d4ae3",
    strip_prefix = "abseil-cpp-20240722.0",
    urls = ["https://github.com/abseil/abseil-cpp/archive/refs/tags/20240722.0.tar.gz"],
)

load("//bazel:arm64_sysroot.bzl", "arm64_sysroot")

# ---------------------------------------------------------------------------
# Git submodule source trees (built from source via rules_foreign_cc).
# After cloning, run: git submodule update --init --recursive
# Build rules live in //third_party:BUILD.bazel.
# ---------------------------------------------------------------------------

_FILEGROUP_BUILD = """
filegroup(
    name = "all",
    srcs = glob(["**"], exclude = ["**/.git/**"]),
    visibility = ["//visibility:public"],
)
"""

new_local_repository(
    name = "libjpeg_turbo",
    path = "third_party/libjpeg-turbo",
    build_file_content = _FILEGROUP_BUILD,
)

new_local_repository(
    name = "libxml2_src",
    path = "third_party/libxml2",
    build_file_content = _FILEGROUP_BUILD,
)

new_local_repository(
    name = "openssl_src",
    path = "third_party/openssl",
    build_file_content = _FILEGROUP_BUILD,
)

new_local_repository(
    name = "curl_src",
    path = "third_party/curl",
    build_file_content = _FILEGROUP_BUILD,
)

# ---------------------------------------------------------------------------
# HTTP archive source trees (downloaded by Bazel on first use, SHA-verified).
# Libraries not hosted on GitHub/GitLab, or whose git history is too large.
# Build rules live in //third_party:BUILD.bazel.
# ---------------------------------------------------------------------------

# GMP 6.3.0 — arbitrary precision arithmetic (dependency of Nettle).
http_archive(
    name = "gmp_src",
    sha256 = "a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898",
    strip_prefix = "gmp-6.3.0",
    urls = ["https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz"],
    build_file_content = _FILEGROUP_BUILD,
)

# libtasn1 4.19.0 — ASN.1 parsing (dependency of GnuTLS).
http_archive(
    name = "libtasn1_src",
    sha256 = "1613f0ac1cf484d6ec0ce3b8c06d56263cc7242f1c23b30d82d23de345a63f7a",
    strip_prefix = "libtasn1-4.19.0",
    urls = ["https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.19.0.tar.gz"],
    build_file_content = _FILEGROUP_BUILD,
)

# Nettle 3.9.1 — symmetric crypto + public-key (dependency of GnuTLS).
http_archive(
    name = "nettle_src",
    sha256 = "ccfeff981b0ca71bbd6fbcb054f407c60ffb644389a5be80d6716d5b550c6ce3",
    strip_prefix = "nettle-3.9.1",
    urls = ["https://ftp.gnu.org/gnu/nettle/nettle-3.9.1.tar.gz"],
    build_file_content = _FILEGROUP_BUILD,
)

# GNU libmicrohttpd 1.0.1 — embedded HTTP server (test-only fake camera).
# Built without HTTPS (--disable-https) so GnuTLS/GMP/Nettle/tasn1 are not
# required at all; only pthreads (always dynamic/system) is needed.
http_archive(
    name = "libmicrohttpd_src",
    sha256 = "a89e09fc9b4de34dde19f4fcb4faaa1ce10299b9908db1132bbfa1de47882b94",
    strip_prefix = "libmicrohttpd-1.0.1",
    urls = ["https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-1.0.1.tar.gz"],
    build_file_content = _FILEGROUP_BUILD,
)

# PostgreSQL 16.13 source — we build only the client library subset:
# libpq (client API), libpgcommon (shared utilities), libpgport (portability).
# Built with --without-gssapi (no Kerberos) and --without-ldap (no LDAP).
http_archive(
    name = "postgres_src",
    sha256 = "9b767d0dfd156424b0b8f02b65eebb4b6958ef6413ebf7c8349e28b0b91e6b09",
    strip_prefix = "postgresql-16.13",
    urls = ["https://ftp.postgresql.org/pub/source/v16.13/postgresql-16.13.tar.gz"],
    build_file_content = _FILEGROUP_BUILD,
)

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
# Packages are downloaded on first use and cached in Bazel's output base.
arm64_sysroot(name = "arm64_sysroot")
