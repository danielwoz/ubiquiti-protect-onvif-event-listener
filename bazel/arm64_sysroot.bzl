"""Repository rule that builds an arm64 sysroot for cross-compilation.

Downloads Ubuntu 22.04 Jammy arm64 packages (no root required) and assembles
a sysroot that clang can use with -target aarch64-linux-gnu.

The resulting binary requires glibc >= 2.35 at runtime.  Jammy is the oldest
base whose glibc is >= 2.34, where libpthread was folded into libc; building
against an older glibc emits a GLIBC_PRIVATE reference against libpthread.so.0
that cannot be satisfied on glibc >= 2.34 hosts such as UniFi OS 6.x (trixie).

Usage in WORKSPACE:
    load("//bazel:arm64_sysroot.bzl", "arm64_sysroot")
    arm64_sysroot(name = "arm64_sysroot")
    register_toolchains("@arm64_sysroot//:aarch64_toolchain")
"""

# ---------------------------------------------------------------------------
# Package list: (url, sha256)
# All packages are Ubuntu 22.04 Jammy from ports.ubuntu.com.
# Cross packages (_all.deb) are mirrored on ports.ubuntu.com alongside the
# native arm64 packages, so a single base URL covers everything.
# ---------------------------------------------------------------------------

_PORTS = "http://ports.ubuntu.com/ubuntu-ports/"

_PACKAGES = [
    # --- Cross-compilation toolchain (glibc 2.35, gcc-11) ------------------
    (_PORTS + "pool/main/c/cross-toolchain-base/libc6-dev-arm64-cross_2.35-0ubuntu1cross3_all.deb",
     "1c552b83243bc5ac6b1ac01078f2396d0f483e849ec0b577bce28e067f6bd65d"),
    (_PORTS + "pool/main/c/cross-toolchain-base/linux-libc-dev-arm64-cross_5.15.0-22.22cross3_all.deb",
     "5f62ce5bee284a4c02ecb68bddcd96334ed71eda7e87c1638fac47e4f58f098d"),
    (_PORTS + "pool/main/c/cross-toolchain-base/libc6-arm64-cross_2.35-0ubuntu1cross3_all.deb",
     "32b6264502998dd170d21eff60b6686be8c682012df18e6cc9fdffcf42c5faae"),
    (_PORTS + "pool/main/g/gcc-11-cross/libgcc-11-dev-arm64-cross_11.4.0-1ubuntu1~22.04.3cross1_all.deb",
     "bc16d649bfdfdb7fe505ec9ef9853463f4f379dad5459cffa3ff5cdf477c817e"),
    (_PORTS + "pool/main/g/gcc-12-cross/libgcc-s1-arm64-cross_12.3.0-1ubuntu1~22.04.3cross1_all.deb",
     "2a62fd8d05a8e1a934f781d4d608a41e6bc12673723c9c2f0e546f30313077c8"),
    (_PORTS + "pool/main/g/gcc-11-cross/libstdc++-11-dev-arm64-cross_11.4.0-1ubuntu1~22.04.3cross1_all.deb",
     "ad4547e19f4c5c0a8d1a483db63346a7079ce261371b8ff8c8a896665d1b8ff8"),
    # --- Native arm64 runtime libraries ------------------------------------
    (_PORTS + "pool/main/libj/libjpeg-turbo/libjpeg-turbo8_2.1.2-0ubuntu1_arm64.deb",
     "1c47447261097e6a2105f4ae6f0bf2c7b1e8c6b8209c8ea40286c69c20f43818"),
    (_PORTS + "pool/main/libx/libxml2/libxml2_2.9.13+dfsg-1ubuntu0.12_arm64.deb",
     "6feb8502740e786e7d6a8d89e3d8d1060aaf6f27ad48bf279cb3c19b451eb16f"),
    (_PORTS + "pool/main/c/curl/libcurl4_7.81.0-1ubuntu1.27_arm64.deb",
     "52644a5b5c205d85ef9b61ca1472f2ee7c770c613ef70fc2a6be97353a0d2d4a"),
    (_PORTS + "pool/main/o/openssl/libssl3_3.0.2-0ubuntu1.29_arm64.deb",
     "3b25897180b0a84a7c7ad19ba310d6bdb45d5ce89742b478e12f606bed38fafa"),
    (_PORTS + "pool/universe/libm/libmicrohttpd/libmicrohttpd12_0.9.75-3ubuntu1_arm64.deb",
     "1f767b443f1b8b84f285611fba8f2757dbd2c96919e22333b0eee85e6f736b32"),
    (_PORTS + "pool/main/p/postgresql-14/libpq5_14.24-0ubuntu0.22.04.1_arm64.deb",
     "61950dd6c8ed2e1ec994bcbdb4634cf590549d42a58d28cc40cced60eec52078"),
    # libicu70: libxml2 runtime depends on it; also provides unicode/ headers
    (_PORTS + "pool/main/i/icu/libicu70_70.1-2_arm64.deb",
     "ac68372cf4a976e6a206858fd9b28c68e49d37d650b9b8653270038a6e7bc174"),
    # --- Native arm64 dev packages (headers + stub .so) --------------------
    (_PORTS + "pool/main/libj/libjpeg-turbo/libjpeg-turbo8-dev_2.1.2-0ubuntu1_arm64.deb",
     "0818819dff2011fd38bd1b4fd9de8238050099993cea41e89f55a818d2ad2db3"),
    (_PORTS + "pool/main/libx/libxml2/libxml2-dev_2.9.13+dfsg-1ubuntu0.12_arm64.deb",
     "36703f65108410a97cc86e36cd1e63c5906dbd645becc7f276af963daa83d9fd"),
    (_PORTS + "pool/main/c/curl/libcurl4-openssl-dev_7.81.0-1ubuntu1.27_arm64.deb",
     "9dd420eb15ccc5111437200ae747143964b4c8967912ddb185e148f325ba2736"),
    (_PORTS + "pool/main/o/openssl/libssl-dev_3.0.2-0ubuntu1.29_arm64.deb",
     "953af13cba07be06f94df64f78ddc60d3e07dc05c8b99053afe9092fe024c944"),
    (_PORTS + "pool/universe/libm/libmicrohttpd/libmicrohttpd-dev_0.9.75-3ubuntu1_arm64.deb",
     "cbb4206cc2273e7817abad6c02b5025dc33763900e3bc0131312383d275a8de9"),
    (_PORTS + "pool/main/p/postgresql-14/libpq-dev_14.24-0ubuntu0.22.04.1_arm64.deb",
     "a70ad26e28b46aab5eda29ee532fca1f8b2d49d4494676a9da7a718f661c51b0"),
    # libicu-dev: provides unicode/ucnv.h and other ICU headers (libxml2 depends on ICU)
    (_PORTS + "pool/main/i/icu/libicu-dev_70.1-2_arm64.deb",
     "5e0609095ce643b4a18e14a2c9852b13fba37e30e7b7e70a2ee2aebaa60008d8"),
    # --- Transitive static deps for libcurl, libxml2, libmicrohttpd, libpq ---
    # libxml2 needs: zlib, lzma (ICU already included above)
    (_PORTS + "pool/main/z/zlib/zlib1g-dev_1.2.11.dfsg-2ubuntu9.2_arm64.deb",
     "9bdd0a80de3b28e35a5cfe59b9d6f6997f0c2d7a16b168be0de9a0b668962189"),
    (_PORTS + "pool/main/x/xz-utils/liblzma-dev_5.2.5-2ubuntu1.1_arm64.deb",
     "33821ba76d0fa36d5b98c4ce8d75a4630876e8cb0ad2bf6885d8dae8445706cb"),
    # libcurl needs: nghttp2, idn2, rtmp, ssh, psl, zstd, brotli, ldap
    (_PORTS + "pool/main/n/nghttp2/libnghttp2-dev_1.43.0-1ubuntu0.4_arm64.deb",
     "9ebfc6d33c315129cc9b6b770b5b4355735fb92407bff4771072886221c74c19"),
    (_PORTS + "pool/main/libi/libidn2/libidn2-dev_2.3.2-2build1_arm64.deb",
     "eafc142b861934b6e44e6b1b22447ce1b2353238b784c472e90c750661f7d7c7"),
    (_PORTS + "pool/main/r/rtmpdump/librtmp-dev_2.4+20151223.gitfa8646d.1-2build4_arm64.deb",
     "0fd3a70b05175215e51d97d3b432cc713bc6b66ea09b05909529aa0535fccbf4"),
    (_PORTS + "pool/main/libs/libssh/libssh-dev_0.9.6-2ubuntu0.22.04.8_arm64.deb",
     "6c44afa50c9030da5c44914549253b13949ac1de098d56206a1eed1e896f5862"),
    (_PORTS + "pool/main/libp/libpsl/libpsl-dev_0.21.0-1.2build2_arm64.deb",
     "1febae141df7f21637e622b169e76c4da4e6251539c49cc5d133fc42fdcf0449"),
    (_PORTS + "pool/main/libz/libzstd/libzstd-dev_1.4.8+dfsg-3build1_arm64.deb",
     "5382229c585552619e1c75210ef07ee5d0e4230351bf388552e6511d053a19c7"),
    (_PORTS + "pool/main/b/brotli/libbrotli-dev_1.0.9-2build6_arm64.deb",
     "c0bd96256db553ba156a061c7d744833e53209fef2934a2dc47167a93a323f62"),
    # Jammy renames the openldap 2.5 dev package: libldap-dev, not libldap2-dev.
    # libldap2-dev still exists in Jammy but is an arch:all transitional stub
    # that only Depends: libldap-dev -- it ships no headers or libraries, so
    # using it here silently produces a sysroot that fails at -lldap/-llber.
    (_PORTS + "pool/main/o/openldap/libldap-dev_2.5.20+dfsg-0ubuntu0.22.04.1_arm64.deb",
     "ce06ceb2539ffbba7140d656082508e01cc54fd011f482007dd241e2b28c2d58"),
    # libmicrohttpd needs gnutls and its deps: gmp, nettle/hogweed, tasn1, unistring, p11-kit
    (_PORTS + "pool/main/g/gnutls28/libgnutls28-dev_3.7.3-4ubuntu1.9_arm64.deb",
     "ec903794dc386dde11bc80c0e10ad95fcdfc0536b2a63783190ff55b4939863d"),
    (_PORTS + "pool/main/g/gmp/libgmp-dev_6.2.1+dfsg-3ubuntu1_arm64.deb",
     "f9c2f51e6d78f899009e61935af2ed3394951863dea78bee9185a695147550f6"),
    (_PORTS + "pool/main/n/nettle/nettle-dev_3.7.3-1build2_arm64.deb",
     "4fb0b4babd0df9e13df7d7eae1b1b95cda92d18c2af81cfa1c1da570dfb4eb16"),
    (_PORTS + "pool/main/libt/libtasn1-6/libtasn1-6-dev_4.18.0-4ubuntu0.2_arm64.deb",
     "586d2af3db7f19d53a82464294984262fcd24520fb9c26ae0c22beb7c94c705e"),
    (_PORTS + "pool/main/libu/libunistring/libunistring-dev_1.0-1_arm64.deb",
     "947eb5a573739f74be75d4eeb7bde2d962605fbb5321cf0495c5c6cf928528da"),
    (_PORTS + "pool/main/p/p11-kit/libp11-kit-dev_0.24.0-6ubuntu0.1_arm64.deb",
     "7a5da262c67ec1c936d5ad8561018d07ec2d6cc6dfc6131f3852f8c7bcb2e5e2"),
    # libpq needs pgcommon + pgport (not bundled in libpq.a on arm64)
    (_PORTS + "pool/universe/p/postgresql-14/postgresql-server-dev-14_14.24-0ubuntu0.22.04.1_arm64.deb",
     "2ef7e74256e1f062a8457f5efe0b008eca5d906750758e626cf29bb543f07ddc"),
]

# Shell script that assembles the sysroot from extracted packages.
_SETUP_SH = """#!/bin/bash
set -euo pipefail
REPO="$(pwd)"
DEBS="$REPO/debs"
SYSROOT="$REPO/sysroot"
TMP="$REPO/_xtmp"

mkdir -p "$SYSROOT/usr/include" \
         "$SYSROOT/usr/lib/aarch64-linux-gnu" \
         "$SYSROOT/usr/lib/gcc-cross" \
         "$SYSROOT/usr/aarch64-linux-gnu/lib"
mkdir -p "$TMP"

# Extract every .deb into the same staging tree.
for deb in "$DEBS"/*.deb; do
    dpkg-deb --extract "$deb" "$TMP"
done

# 1. C/C++ system headers from libc6-dev-arm64-cross (and libstdc++-dev):
#    cross packages put them at usr/aarch64-linux-gnu/include/
if [ -d "$TMP/usr/aarch64-linux-gnu/include" ]; then
    cp -a "$TMP/usr/aarch64-linux-gnu/include/." "$SYSROOT/usr/include/"
fi

# 2. All libc files (crt .o + stubs + versioned runtime):
#    cross packages use usr/aarch64-linux-gnu/lib/
if [ -d "$TMP/usr/aarch64-linux-gnu/lib" ]; then
    cp -a "$TMP/usr/aarch64-linux-gnu/lib/." "$SYSROOT/usr/aarch64-linux-gnu/lib/"
fi

# 3. gcc runtime (crtbeginS.o, libgcc.a, libstdc++.a, etc.):
if [ -d "$TMP/usr/lib/gcc-cross" ]; then
    cp -a "$TMP/usr/lib/gcc-cross/." "$SYSROOT/usr/lib/gcc-cross/"
fi

# 4. App library headers from native arm64 dev packages (usr/include/):
if [ -d "$TMP/usr/include" ]; then
    cp -a "$TMP/usr/include/." "$SYSROOT/usr/include/"
fi

# 5. App library .so/.a from native arm64 dev+runtime packages:
if [ -d "$TMP/usr/lib/aarch64-linux-gnu" ]; then
    cp -a "$TMP/usr/lib/aarch64-linux-gnu/." "$SYSROOT/usr/lib/aarch64-linux-gnu/"
fi

# 5b. PostgreSQL static libs (libpgcommon.a, libpgport.a) from postgresql-server-dev:
#     They install to usr/lib/postgresql/<major>/lib/ — copy into standard lib
#     dir.  Glob the major version rather than hardcoding it: the directory is
#     named after the server release (12 on Focal, 14 on Jammy), and a stale
#     hardcoded path fails *silently* here and only surfaces much later as
#     "ld.lld: error: unable to find library -lpgcommon".  Fail loudly instead.
_pg_found=0
for _pgdir in "$TMP"/usr/lib/postgresql/*/lib; do
    [ -d "$_pgdir" ] || continue
    mkdir -p "$SYSROOT/usr/lib/aarch64-linux-gnu"
    for a in "$_pgdir"/*.a; do
        [ -f "$a" ] || continue
        cp -a "$a" "$SYSROOT/usr/lib/aarch64-linux-gnu/"
        _pg_found=1
    done
done
if [ "$_pg_found" -ne 1 ]; then
    echo "ERROR: no libpg*.a found under $TMP/usr/lib/postgresql/*/lib" >&2
    echo "       (postgresql-server-dev-* layout changed?)" >&2
    exit 1
fi

# 6. Make gcc runtime visible to clang's --gcc-toolchain discovery.
#    Cross packages put the runtime at usr/lib/gcc-cross/aarch64-linux-gnu/
#    but clang (--gcc-toolchain=$SYSROOT/usr) probes usr/lib/gcc/aarch64-linux-gnu/.
#    A relative symlink bridges the two paths.
if [ -d "$SYSROOT/usr/lib/gcc-cross/aarch64-linux-gnu" ]; then
    mkdir -p "$SYSROOT/usr/lib/gcc"
    ln -sfn "../gcc-cross/aarch64-linux-gnu" "$SYSROOT/usr/lib/gcc/aarch64-linux-gnu"
fi

# 7. Compile GSSAPI stub → libgssapi_krb5.a
#    libcurl and libpq are built with Kerberos support but we never use it.
#    Ubuntu does not ship libgssapi_krb5.a, so we build minimal stubs.
cat > "$REPO/_gssapi_stub.c" << 'GSSAPI_EOF'
typedef unsigned int OM_uint32;
typedef struct { unsigned int length; void *elements; } gss_OID_desc, *gss_OID;
typedef struct { unsigned long count; gss_OID elements; } *gss_OID_set;
typedef void *gss_ctx_id_t, *gss_cred_id_t, *gss_name_t;
typedef struct { unsigned long length; void *value; } gss_buffer_desc, *gss_buffer_t;
#define FAIL 13u
static gss_OID_desc _hbs = {0,0};
gss_OID GSS_C_NT_HOSTBASED_SERVICE = &_hbs;
OM_uint32 gss_acquire_cred(OM_uint32*m,gss_name_t a,OM_uint32 b,gss_OID_set c,int d,gss_cred_id_t*e,gss_OID_set*f,OM_uint32*g){if(m)*m=0;return FAIL;}
OM_uint32 gss_delete_sec_context(OM_uint32*m,gss_ctx_id_t*c,gss_buffer_t t){if(m)*m=0;return 0;}
OM_uint32 gss_display_name(OM_uint32*m,gss_name_t n,gss_buffer_t b,gss_OID*t){if(m)*m=0;return FAIL;}
OM_uint32 gss_display_status(OM_uint32*m,OM_uint32 s,int t,gss_OID mech,OM_uint32*mc,gss_buffer_t b){if(m)*m=0;return FAIL;}
OM_uint32 gss_import_name(OM_uint32*m,gss_buffer_t b,gss_OID t,gss_name_t*n){if(m)*m=0;return FAIL;}
OM_uint32 gss_init_sec_context(OM_uint32*m,gss_cred_id_t c,gss_ctx_id_t*x,gss_name_t t,gss_OID mech,OM_uint32 f,OM_uint32 tl,void*cb,gss_buffer_t it,gss_OID*am,gss_buffer_t ot,OM_uint32*rf,OM_uint32*et){if(m)*m=0;return FAIL;}
OM_uint32 gss_inquire_context(OM_uint32*m,gss_ctx_id_t x,gss_name_t*sn,gss_name_t*tn,OM_uint32*lt,gss_OID*mech,OM_uint32*fl,int*lo,int*op){if(m)*m=0;return FAIL;}
OM_uint32 gss_release_buffer(OM_uint32*m,gss_buffer_t b){if(m)*m=0;if(b){b->length=0;b->value=0;}return 0;}
OM_uint32 gss_release_cred(OM_uint32*m,gss_cred_id_t*c){if(m)*m=0;if(c)*c=0;return 0;}
OM_uint32 gss_release_name(OM_uint32*m,gss_name_t*n){if(m)*m=0;if(n)*n=0;return 0;}
OM_uint32 gss_unwrap(OM_uint32*m,gss_ctx_id_t x,gss_buffer_t ib,gss_buffer_t ob,int*conf,OM_uint32*qop){if(m)*m=0;return FAIL;}
OM_uint32 gss_wrap(OM_uint32*m,gss_ctx_id_t x,int conf,OM_uint32 qop,gss_buffer_t ib,int*cs,gss_buffer_t ob){if(m)*m=0;return FAIL;}
OM_uint32 gss_wrap_size_limit(OM_uint32*m,gss_ctx_id_t x,int conf,OM_uint32 qop,OM_uint32 mo,OM_uint32*mi){if(m)*m=0;if(mi)*mi=0;return FAIL;}
OM_uint32 gss_create_empty_oid_set(OM_uint32*m,gss_OID_set*s){if(m)*m=0;if(s)*s=0;return FAIL;}
OM_uint32 gss_indicate_mechs(OM_uint32*m,gss_OID_set*s){if(m)*m=0;if(s)*s=0;return FAIL;}
OM_uint32 gss_test_oid_set_member(OM_uint32*m,gss_OID o,gss_OID_set s,int*p){if(m)*m=0;if(p)*p=0;return FAIL;}
OM_uint32 gss_add_oid_set_member(OM_uint32*m,gss_OID o,gss_OID_set*s){if(m)*m=0;return FAIL;}
OM_uint32 gss_release_oid_set(OM_uint32*m,gss_OID_set*s){if(m)*m=0;if(s)*s=0;return FAIL;}
OM_uint32 gss_oid_to_str(OM_uint32*m,gss_OID o,gss_buffer_t b){if(m)*m=0;return FAIL;}
OM_uint32 gss_str_to_oid(OM_uint32*m,gss_buffer_t b,gss_OID*o){if(m)*m=0;if(o)*o=0;return FAIL;}
OM_uint32 gss_accept_sec_context(OM_uint32*m,gss_ctx_id_t*x,gss_cred_id_t c,gss_buffer_t it,void*cb,gss_name_t*sn,gss_OID*am,gss_buffer_t ot,OM_uint32*rf,OM_uint32*et,gss_cred_id_t*del){if(m)*m=0;return FAIL;}
OM_uint32 gss_verify_mic(OM_uint32*m,gss_ctx_id_t x,gss_buffer_t mb,gss_buffer_t tk,OM_uint32*qop){if(m)*m=0;return FAIL;}
OM_uint32 gss_get_mic(OM_uint32*m,gss_ctx_id_t x,OM_uint32 qop,gss_buffer_t mb,gss_buffer_t tk){if(m)*m=0;return FAIL;}
OM_uint32 gss_inquire_cred(OM_uint32*m,gss_cred_id_t c,gss_name_t*n,OM_uint32*lt,int*u,gss_OID_set*ms){if(m)*m=0;return FAIL;}
OM_uint32 gss_inquire_cred_by_mech(OM_uint32*m,gss_cred_id_t c,gss_OID mech,gss_name_t*n,OM_uint32*il,OM_uint32*al,int*u){if(m)*m=0;return FAIL;}
static gss_OID_desc _un = {0,0};
gss_OID GSS_C_NT_USER_NAME = &_un;
GSSAPI_EOF
/usr/bin/clang -target aarch64-linux-gnu --sysroot="$SYSROOT" \
    -c -O2 "$REPO/_gssapi_stub.c" -o "$REPO/_gssapi_stub.o"
ar rcs "$SYSROOT/usr/lib/aarch64-linux-gnu/libgssapi_krb5.a" "$REPO/_gssapi_stub.o"
rm -f "$REPO/_gssapi_stub.c" "$REPO/_gssapi_stub.o"
echo "GSSAPI stub built"

# 8. Compile p11-kit stub → libp11-kit.a
#    libgnutls was built with p11-kit support; Ubuntu ships only .so, not .a.
cat > "$REPO/_p11kit_stub.c" << 'P11KIT_EOF'
#include <stddef.h>
void* p11_kit_module_load(const char* n, unsigned long f) { return 0; }
int p11_kit_module_initialize(void* m) { return -1; }
int p11_kit_module_finalize(void* m) { return 0; }
void p11_kit_module_release(void* m) {}
void** p11_kit_modules_load_and_initialize(unsigned long f) { return 0; }
unsigned long p11_kit_module_get_flags(void* m) { return 0; }
char* p11_kit_module_get_name(void* m) { return 0; }
const char* p11_kit_strerror(int rv) { return "stub"; }
char* p11_kit_config_option(void* m, const char* k) { return 0; }
const char* p11_kit_message(void) { return 0; }
typedef struct { int d; } P11KitPin;
typedef int (*P11KitPinCB)(const char*,void*,void*,unsigned int,P11KitPin**);
P11KitPin* p11_kit_pin_new_for_string(const char* s) { return 0; }
const unsigned char* p11_kit_pin_get_value(P11KitPin* p, size_t* l) { if(l)*l=0; return 0; }
size_t p11_kit_pin_get_length(P11KitPin* p) { return 0; }
void p11_kit_pin_unref(P11KitPin* p) {}
int p11_kit_pin_register_callback(const char* u, P11KitPinCB cb, void* d, void(*f)(void*)) { return -1; }
void p11_kit_pin_unregister_callback(const char* u, P11KitPinCB cb, void* d) {}
P11KitPin* p11_kit_pin_request(const char* u, void* ps, void* ti, unsigned int f) { return 0; }
int p11_kit_pin_file_callback(const char* p, void* s, void* t, unsigned int f, P11KitPin** r) { if(r)*r=0; return -1; }
typedef struct { int d; } P11KitUri;
P11KitUri* p11_kit_uri_new(void) { return 0; }
void p11_kit_uri_free(P11KitUri* u) {}
int p11_kit_uri_parse(const char* s, unsigned long t, P11KitUri* u) { return -1; }
int p11_kit_uri_format(P11KitUri* u, unsigned long t, char** s) { if(s)*s=0; return -1; }
void* p11_kit_uri_get_module_info(P11KitUri* u) { return 0; }
void* p11_kit_uri_get_token_info(P11KitUri* u) { return 0; }
void* p11_kit_uri_get_attributes(P11KitUri* u, size_t* n) { if(n)*n=0; return 0; }
void* p11_kit_uri_get_attribute(P11KitUri* u, unsigned long t) { return 0; }
int p11_kit_uri_set_attribute(P11KitUri* u, void* a) { return -1; }
int p11_kit_uri_match_module_info(P11KitUri* u, void* i) { return 0; }
int p11_kit_uri_match_token_info(P11KitUri* u, void* i) { return 0; }
const char* p11_kit_uri_get_pin_source(P11KitUri* u) { return 0; }
void* p11_kit_uri_get_pin_value(P11KitUri* u) { return 0; }
char* p11_kit_space_strdup(const char* s, size_t l) { return 0; }
size_t p11_kit_space_strlen(const char* s, size_t l) { return 0; }
P11KIT_EOF
/usr/bin/clang -target aarch64-linux-gnu --sysroot="$SYSROOT" \
    -c -O2 "$REPO/_p11kit_stub.c" -o "$REPO/_p11kit_stub.o"
ar rcs "$SYSROOT/usr/lib/aarch64-linux-gnu/libp11-kit.a" "$REPO/_p11kit_stub.o"
rm -f "$REPO/_p11kit_stub.c" "$REPO/_p11kit_stub.o"
echo "p11-kit stub built"

# 9. Compile Cyrus SASL stub → libsasl2.a
#    libcurl/libldap are built with SASL support but we never exercise SASL auth.
#    Ubuntu's libsasl2.a compiles in Berkeley DB and MySQL backends, pulling in
#    libdb/libmariadb deps; a minimal stub avoids all of that.
cat > "$REPO/_sasl_stub.c" << 'SASL_EOF'
#include <stddef.h>
int sasl_client_init(const void *cbs) { return -4; }
int sasl_client_new(const char *s, const char *h, const char *l, const char *r,
                    const void *c, unsigned f, void **p) { if(p)*p=0; return -4; }
void sasl_dispose(void **p) { if(p)*p=0; }
int sasl_client_start(void *c, const char *m, void **p, const char **o,
                      unsigned *ol, const char **mc) { return -4; }
int sasl_client_step(void *c, const char *in, unsigned il, void **p,
                     const char **o, unsigned *ol) { return -4; }
int sasl_encode(void *c, const char *in, unsigned il,
                const char **o, unsigned *ol) { return -4; }
int sasl_decode(void *c, const char *in, unsigned il,
                const char **o, unsigned *ol) { return -4; }
int sasl_getprop(void *c, int n, const void **v) { if(v)*v=0; return -4; }
int sasl_setprop(void *c, int n, const void *v) { return -4; }
const char *sasl_errstring(int e, const char *l, const char **o) { return "sasl stub"; }
const char *sasl_errdetail(void *c) { return "sasl stub"; }
void sasl_set_mutex(void *a, void *b, void *c, void *d) {}
const char **sasl_global_listmech(void) { return 0; }
SASL_EOF
/usr/bin/clang -target aarch64-linux-gnu --sysroot="$SYSROOT" \
    -c -O2 "$REPO/_sasl_stub.c" -o "$REPO/_sasl_stub.o"
ar rcs "$SYSROOT/usr/lib/aarch64-linux-gnu/libsasl2.a" "$REPO/_sasl_stub.o"
rm -f "$REPO/_sasl_stub.c" "$REPO/_sasl_stub.o"
echo "SASL stub built"

# 10. Build LLVM profile runtime for aarch64.
#     libclang_rt.profile-aarch64.a is needed for --config=pgo_instrument ARM64 builds.
#     Ubuntu omits the aarch64 cross-compilation runtime, so we build it from the
#     matching LLVM source (version detected at repository-rule time in arm64_sysroot.bzl).
echo "Building ARM64 LLVM profile runtime..."
mkdir -p "$REPO/resource_dir/include"
mkdir -p "$REPO/resource_dir/lib/linux"

# Copy clang builtin headers into our local resource directory so the wrapper
# can use --resource-dir and keep everything self-contained.
CLANG_RT_DIR=$(clang --print-resource-dir)
cp -r "$CLANG_RT_DIR/include/." "$REPO/resource_dir/include/"

# Profile source files were pre-downloaded by rctx.download() into profile_rt_src/.
PROFILE_SRC="$REPO/profile_rt_src"

# Cross-compile for aarch64. Only PlatformLinux.c is correct for Linux;
# the Darwin/AIX/Fuchsia/Windows platform files are omitted to avoid
# pulling in platform-specific headers that don't exist on Linux.
CFLAGS="-target aarch64-linux-gnu --sysroot=$SYSROOT --gcc-toolchain=$SYSROOT/usr -O2 -fPIC -I$PROFILE_SRC -DCOMPILER_RT_HAS_UNAME"
PROFILE_OBJS=()
for f in GCDAProfiling.c InstrProfiling.c InstrProfilingBuffer.c \
          InstrProfilingFile.c InstrProfilingInternal.c \
          InstrProfilingMerge.c InstrProfilingMergeFile.c \
          InstrProfilingNameVar.c InstrProfilingPlatformLinux.c \
          InstrProfilingUtil.c InstrProfilingValue.c \
          InstrProfilingVersionVar.c InstrProfilingWriter.c; do
    /usr/bin/clang $CFLAGS -c "$PROFILE_SRC/$f" -o "$PROFILE_SRC/${f%.c}.o"
    PROFILE_OBJS+=("$PROFILE_SRC/${f%.c}.o")
done
/usr/bin/clang++ $CFLAGS -std=c++11 \
    -c "$PROFILE_SRC/InstrProfilingRuntime.cpp" \
    -o "$PROFILE_SRC/InstrProfilingRuntime.o"
PROFILE_OBJS+=("$PROFILE_SRC/InstrProfilingRuntime.o")
ar rcs "$REPO/resource_dir/lib/linux/libclang_rt.profile-aarch64.a" "${PROFILE_OBJS[@]}"
rm -rf "$PROFILE_SRC"
echo "ARM64 profile runtime built"

rm -rf "$TMP"
echo "arm64 sysroot built at $SYSROOT"
"""

# Wrapper script template: the script computes its own location so the
# sysroot path is correct regardless of Bazel's output base.
_CLANG_WRAPPER = """#!/bin/bash
SCRIPT="$(readlink -f "${{BASH_SOURCE[0]}}")"
DIR="$(dirname "$SCRIPT")"
SYSROOT="$DIR/../sysroot"
RESOURCE_DIR="$DIR/../resource_dir"
exec /usr/bin/clang{suffix} \\
  -target aarch64-linux-gnu \\
  --sysroot="$SYSROOT" \\
  --gcc-toolchain="$SYSROOT/usr" \\
  -resource-dir="$RESOURCE_DIR" \\
  -fuse-ld=lld \\
  -Wl,-Bstatic \\
  "$@" \\
  -Wl,-Bdynamic
"""

def _arm64_sysroot_impl(rctx):
    # 1. Download all packages.
    for url, sha256 in _PACKAGES:
        filename = url.split("/")[-1]
        rctx.download(
            url = url,
            output = "debs/" + filename,
            sha256 = sha256,
        )

    # 2. Pre-download compiler-rt profile runtime sources matching the installed
    #    clang version.  rctx.download() has network access; bash execute() does not.
    #    The profile runtime source must match the clang binary: between LLVM 14 and
    #    15+ the __llvm_profile_header struct was reorganised (DataSize/CountersSize
    #    fields were removed), so using LLVM 14 sources with clang-15+ fails to compile.
    #    We detect the clang major version at repository-rule time and download the
    #    matching llvmorg-X.0.0 tag.  Falls back to 14 if detection fails.
    clang_major = 14
    clang_ver = rctx.execute(["/usr/bin/clang", "--version"])
    if clang_ver.return_code == 0:
        marker = "version "
        idx = clang_ver.stdout.find(marker)
        if idx >= 0:
            rest = clang_ver.stdout[idx + len(marker):]
            major_str = rest.split(".")[0].strip()
            if len(major_str) > 0 and major_str[0] >= "1" and major_str[0] <= "9":
                clang_major = int(major_str)
    # Use the release/X.x branch (e.g. release/18.x) rather than a specific tag:
    # LLVM 14-17 start at X.0.0 but LLVM 18+ start at X.1.0, so "llvmorg-X.0.0"
    # does not exist for newer versions.  The release/X.x branch always exists and
    # tracks the latest patch of that major release.
    _llvm_base = (
        "https://raw.githubusercontent.com/llvm/llvm-project/release/{}.x".format(clang_major) +
        "/compiler-rt/lib/profile/"
    )
    for f in [
        "InstrProfiling.h",
        "InstrProfilingInternal.h",
        "InstrProfilingPort.h",
        "InstrProfilingUtil.h",
        "GCDAProfiling.c",
        "InstrProfiling.c",
        "InstrProfilingBuffer.c",
        "InstrProfilingFile.c",
        "InstrProfilingInternal.c",
        "InstrProfilingMerge.c",
        "InstrProfilingMergeFile.c",
        "InstrProfilingNameVar.c",
        "InstrProfilingPlatformLinux.c",
        "InstrProfilingRuntime.cpp",
        "InstrProfilingUtil.c",
        "InstrProfilingValue.c",
        "InstrProfilingVersionVar.c",
        "InstrProfilingWriter.c",
    ]:
        rctx.download(url = _llvm_base + f, output = "profile_rt_src/" + f)

    # 3. Build the sysroot.
    rctx.file("setup.sh", content = _SETUP_SH, executable = True)
    result = rctx.execute(["bash", "setup.sh"], timeout = 600)
    if result.return_code != 0:
        fail("arm64_sysroot setup.sh failed:\nstdout:\n{}\nstderr:\n{}".format(
            result.stdout, result.stderr))

    # 4. Create compiler wrapper scripts.
    rctx.file("bin/clang_arm64",
              content = _CLANG_WRAPPER.format(suffix = ""),
              executable = True)
    rctx.file("bin/clang++_arm64",
              content = _CLANG_WRAPPER.format(suffix = "++"),
              executable = True)

    # 5. Compute sysroot and resource_dir paths for toolchain config.
    #    The resource_dir is built by _SETUP_SH and contains our local copy of
    #    the clang builtin headers and the cross-compiled profile runtime.
    #    Using --resource-dir in the wrapper keeps all clang-internal paths
    #    self-contained and avoids fragile host-path matching.
    sysroot = str(rctx.path("sysroot"))
    resource_dir = str(rctx.path("resource_dir"))

    # 6. Generate the Starlark toolchain config (needs a .bzl file because
    #    rule() is not allowed in BUILD files).
    rctx.file("toolchain_config.bzl", content = """
\"\"\"Auto-generated aarch64-linux-gnu CC toolchain config.\"\"\"
load(
    "@bazel_tools//tools/cpp:cc_toolchain_config_lib.bzl",
    "action_config",
    "feature",
    "flag_group",
    "flag_set",
    "tool",
    "tool_path",
    "with_feature_set",
)

ALL_COMPILE_ACTIONS = [
    "c-compile",
    "c++-compile",
    "c++-header-parsing",
    "c++-module-compile",
    "assemble",
    "preprocess-assemble",
    "lto-backend",
    "clif-match",
]
ALL_LINK_ACTIONS = [
    "c++-link-executable",
    "c++-link-dynamic-library",
    "c++-link-nodeps-dynamic-library",
]

def _aarch64_toolchain_config_impl(ctx):
    sysroot = "{sysroot}"

    tool_paths = [
        tool_path(name = "gcc",     path = "bin/clang_arm64"),
        tool_path(name = "ld",      path = "bin/clang_arm64"),
        tool_path(name = "cpp",     path = "bin/clang++_arm64"),
        tool_path(name = "ar",      path = "/usr/bin/ar"),
        tool_path(name = "nm",      path = "/usr/bin/nm"),
        tool_path(name = "objdump", path = "/usr/bin/objdump"),
        tool_path(name = "strip",   path = "/usr/bin/strip"),
        tool_path(name = "gcov",    path = "/usr/bin/gcov"),
    ]

    # Features are enabled by default (enabled=True) so they apply to every
    # action without the build file needing to mention them.
    default_flags = feature(
        name = "default_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = ALL_COMPILE_ACTIONS,
                flag_groups = [flag_group(flags = [
                    "-no-canonical-prefixes",
                    "-fno-omit-frame-pointer",
                    # libxml2 headers live under libxml2/libxml/ but are
                    # included as <libxml/parser.h>, so we need -isystem.
                    "-isystem", "{sysroot}/usr/include/libxml2",
                    # libpq-fe.h lives under postgresql/ subdirectory and is
                    # included as <libpq-fe.h> (no prefix), so we need -isystem.
                    "-isystem", "{sysroot}/usr/include/postgresql",
                ])],
            ),
        ],
    )

    # Explicit C++ runtime link flags.  Bazel does not add -lstdc++ automatically
    # for custom toolchains; we must request it.  Order matters: user libs first,
    # then the C++ runtime, then gcc support, then libc.
    link_flags = feature(
        name = "link_flags",
        enabled = True,
        flag_sets = [
            flag_set(
                actions = ALL_LINK_ACTIONS,
                flag_groups = [flag_group(flags = [
                    "-lstdc++",
                    "-lgcc",
                    "-Wl,-Bdynamic",
                    "-lm",
                    "-ldl",
                    "-lc",
                ])],
            ),
        ],
    )

    supports_pic = feature(name = "supports_pic", enabled = True)
    supports_dynamic_linker = feature(name = "supports_dynamic_linker", enabled = True)

    return cc_common.create_cc_toolchain_config_info(
        ctx = ctx,
        toolchain_identifier   = "aarch64-linux-gnu-clang",
        host_system_name       = "x86_64-unknown-linux-gnu",
        target_system_name     = "aarch64-unknown-linux-gnu",
        target_cpu             = "aarch64",
        target_libc            = "glibc_2.35",
        compiler               = "clang",
        abi_version            = "aarch64",
        abi_libc_version       = "glibc_2.35",
        tool_paths             = tool_paths,
        features               = [default_flags, link_flags, supports_pic, supports_dynamic_linker],
        # Tell Bazel where system headers live so it tracks them as inputs.
        cxx_builtin_include_directories = [
            sysroot + "/usr/include",
            sysroot + "/usr/include/c++/11",
            sysroot + "/usr/include/c++/11/backward",
            sysroot + "/usr/include/aarch64-linux-gnu",
            sysroot + "/usr/include/c++/11/aarch64-linux-gnu",
            # libxml2 headers are under libxml2/ (included as <libxml/...>)
            sysroot + "/usr/include/libxml2",
            # libpq-fe.h is under a postgresql/ subdirectory
            sysroot + "/usr/include/postgresql",
            # clang builtin headers from our self-contained resource directory
            # (built by _SETUP_SH alongside the profile runtime).  Using a
            # local copy avoids fragile host-path matching across distros.
            "{resource_dir}/include",
        ],
    )

aarch64_toolchain_config = rule(
    implementation = _aarch64_toolchain_config_impl,
    provides = [CcToolchainConfigInfo],
    attrs = {{}},
)
""".format(sysroot = sysroot, resource_dir = resource_dir))

    # 7. Generate BUILD.bazel.
    rctx.file("BUILD.bazel", content = """
load(":toolchain_config.bzl", "aarch64_toolchain_config")

package(default_visibility = ["//visibility:public"])

filegroup(
    name = "all_files",
    srcs = glob(["bin/**", "sysroot/**", "resource_dir/**"]),
)

filegroup(name = "empty", srcs = [])

aarch64_toolchain_config(name = "aarch64_toolchain_config")

cc_toolchain(
    name = "aarch64_cc_toolchain",
    toolchain_config      = ":aarch64_toolchain_config",
    all_files             = ":all_files",
    compiler_files        = ":all_files",
    linker_files          = ":all_files",
    ar_files              = ":all_files",
    objcopy_files         = ":empty",
    strip_files           = ":empty",
    dwp_files             = ":empty",
    supports_param_files  = 0,
)

toolchain(
    name = "aarch64_toolchain",
    exec_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:x86_64",
    ],
    target_compatible_with = [
        "@platforms//os:linux",
        "@platforms//cpu:aarch64",
    ],
    toolchain       = ":aarch64_cc_toolchain",
    toolchain_type  = "@bazel_tools//tools/cpp:toolchain_type",
)

# Stub cc_library targets for arm64 builds.  Headers come from the sysroot
# (declared in cxx_builtin_include_directories); we only need the link flags.
# The clang wrapper injects -Wl,-Bstatic before "$@" and -Wl,-Bdynamic after,
# so these -lXXX flags resolve to .a files in the sysroot.
cc_library(name = "libxml2",       linkopts = ["-lxml2", "-licui18n", "-licuuc", "-licudata", "-llzma", "-lz"])
cc_library(name = "libcurl",       linkopts = ["-Wl,--allow-multiple-definition", "-lcurl", "-lnghttp2", "-lidn2", "-lunistring", "-lrtmp", "-lssh", "-lpsl", "-lzstd", "-lbrotlidec", "-lbrotlicommon", "-lldap", "-llber", "-lsasl2", "-lgnutls", "-lhogweed", "-lnettle", "-lgmp", "-ltasn1", "-lp11-kit", "-lgssapi_krb5", "-lssl", "-lcrypto", "-lz"])
cc_library(name = "openssl",       linkopts = ["-lssl", "-lcrypto"])
cc_library(name = "libmicrohttpd", linkopts = ["-lmicrohttpd", "-lgnutls", "-lhogweed", "-lnettle", "-lgmp", "-ltasn1", "-lunistring", "-lp11-kit"])
cc_library(name = "libpq",         linkopts = ["-lpq", "-lpgcommon", "-lpgport", "-lgssapi_krb5", "-lssl", "-lcrypto"])
cc_library(name = "libjpeg",       linkopts = ["-ljpeg"])
""")

arm64_sysroot = repository_rule(
    implementation = _arm64_sysroot_impl,
    attrs = {},
    local = False,
    doc = "Downloads Ubuntu arm64 packages and builds a sysroot for cross-compilation.",
)
