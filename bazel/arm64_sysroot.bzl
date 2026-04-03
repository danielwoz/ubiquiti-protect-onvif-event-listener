"""Repository rule that builds an arm64 sysroot for cross-compilation.

Downloads Ubuntu 20.04 Focal arm64 packages (no root required) and assembles
a sysroot that clang can use with -target aarch64-linux-gnu.

The resulting binary requires only glibc >= 2.31 at runtime, matching the
glibc version shipped on Ubuntu 20.04 / Ubiquiti devices.

Usage in WORKSPACE:
    load("//bazel:arm64_sysroot.bzl", "arm64_sysroot")
    arm64_sysroot(name = "arm64_sysroot")
    register_toolchains("@arm64_sysroot//:aarch64_toolchain")
"""

# ---------------------------------------------------------------------------
# Package list: (url, sha256)
# All packages are Ubuntu 20.04 Focal from ports.ubuntu.com.
# Cross packages (_all.deb) are mirrored on ports.ubuntu.com alongside the
# native arm64 packages, so a single base URL covers everything.
# ---------------------------------------------------------------------------

_PORTS = "http://ports.ubuntu.com/ubuntu-ports/"

_PACKAGES = [
    # --- Cross-compilation toolchain (glibc 2.31, gcc-9) -------------------
    (_PORTS + "pool/main/c/cross-toolchain-base/libc6-dev-arm64-cross_2.31-0ubuntu9.9cross1_all.deb",
     "fea8e2c2c74360e2ddc53bd4dfefbbbecc089c1def2eaf5a0eb1d5f04b59388c"),
    (_PORTS + "pool/main/c/cross-toolchain-base/linux-libc-dev-arm64-cross_5.4.0-110.124cross1_all.deb",
     "fc4ae3b72715906727565d29042bbac08ce4e46ca43b4b3d16a372e3813bf45f"),
    (_PORTS + "pool/main/c/cross-toolchain-base/libc6-arm64-cross_2.31-0ubuntu9.9cross1_all.deb",
     "d37029619ca1d94c37e787c4d7f08274dea82f3061f6d2a8239bd14e7371c99f"),
    (_PORTS + "pool/main/g/gcc-9-cross/libgcc-9-dev-arm64-cross_9.4.0-1ubuntu1~20.04.2cross2_all.deb",
     "efdab974d7271864bedf5f6cd74cd7236bce02e293174f57c54fbbed19736899"),
    (_PORTS + "pool/main/g/gcc-10-cross/libgcc-s1-arm64-cross_10.5.0-1ubuntu1~20.04cross1_all.deb",
     "9065f00c2a8dc8baf4584355f7eb3b18840212e62ea7eff68dab30a0ab250d59"),
    (_PORTS + "pool/main/g/gcc-9-cross/libstdc++-9-dev-arm64-cross_9.4.0-1ubuntu1~20.04.2cross2_all.deb",
     "17b0294c70127eeeaa38a334f4fd13971bdf9c6a26a26489f9ea3e049a0fc848"),
    # --- Native arm64 runtime libraries ------------------------------------
    (_PORTS + "pool/main/libj/libjpeg-turbo/libjpeg-turbo8_2.0.3-0ubuntu1_arm64.deb",
     "cd6588ac233b9c1385ae6490e1969f15fe87b88b4f554dff0504eb4067edeb63"),
    (_PORTS + "pool/main/libx/libxml2/libxml2_2.9.10+dfsg-5ubuntu0.20.04.10_arm64.deb",
     "709c2d78a585c0280296f9d0136c71319ba4dcf06e72ffc18687747771ea2009"),
    (_PORTS + "pool/main/c/curl/libcurl4_7.68.0-1ubuntu2.25_arm64.deb",
     "ac1248efbe550c89838ec178ff560b0aabb64071924131a59ea9f54713ee6c35"),
    (_PORTS + "pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2.24_arm64.deb",
     "dded4572af8b0a9e0310909f211a519cc6409fda31ea81132a77e268b0ec0f2f"),
    (_PORTS + "pool/universe/libm/libmicrohttpd/libmicrohttpd12_0.9.66-1_arm64.deb",
     "00ecf8da0d7595ff15c3f171cc774f08a9b6e28883417529d59b7874b11b01f0"),
    (_PORTS + "pool/main/p/postgresql-12/libpq5_12.22-0ubuntu0.20.04.4_arm64.deb",
     "87b2632f4530f5d36a50d9ceea5af84d6500c486dbb4e5a1cf19cab549d70aac"),
    # libicu66: libxml2 runtime depends on it; also provides unicode/ headers
    (_PORTS + "pool/main/i/icu/libicu66_66.1-2ubuntu2.1_arm64.deb",
     "ccfbd8e68fadcf4cc3975cdae42d1786733963f75d0b180f96c13782e185635b"),
    # --- Native arm64 dev packages (headers + stub .so) --------------------
    (_PORTS + "pool/main/libj/libjpeg-turbo/libjpeg-turbo8-dev_2.0.3-0ubuntu1_arm64.deb",
     "b2f20f3cc2147e387718ab3e3762833585de172353a9fca7c5f64842ab74304a"),
    (_PORTS + "pool/main/libx/libxml2/libxml2-dev_2.9.10+dfsg-5ubuntu0.20.04.10_arm64.deb",
     "0f222bdda6831bfa605b7c0fbbdce26c26bb844d34de1430b177ff9c9a5a9c92"),
    (_PORTS + "pool/main/c/curl/libcurl4-openssl-dev_7.68.0-1ubuntu2.25_arm64.deb",
     "499d2a6526765266d79d8f10169078ae4538d9333225afc595d59f9a9b238596"),
    (_PORTS + "pool/main/o/openssl/libssl-dev_1.1.1f-1ubuntu2.24_arm64.deb",
     "a664d282a0b19fb687c1b9f5a06abcf879bc5643c6e8be435f13c187d13a5b6a"),
    (_PORTS + "pool/universe/libm/libmicrohttpd/libmicrohttpd-dev_0.9.66-1_arm64.deb",
     "f39debe221a2424e3aca23dca099e07c766d5a161aa399c579bb1ef8c186ebd0"),
    (_PORTS + "pool/main/p/postgresql-12/libpq-dev_12.22-0ubuntu0.20.04.4_arm64.deb",
     "2790457020356cda2229865c5c54cbbc99e14288bfb58e25d33422e9f9ef51fa"),
    # libicu-dev: provides unicode/ucnv.h and other ICU headers (libxml2 depends on ICU)
    (_PORTS + "pool/main/i/icu/libicu-dev_66.1-2ubuntu2.1_arm64.deb",
     "c19f2637b20756d61f347116cadcac4eb0078922bbcec9d4fddbab1987233178"),
    # --- Transitive static deps for libcurl, libxml2, libmicrohttpd, libpq ---
    # libxml2 needs: zlib, lzma (ICU already included above)
    (_PORTS + "pool/main/z/zlib/zlib1g-dev_1.2.11.dfsg-2ubuntu1.5_arm64.deb",
     "aca13b896f90ca536831ccbd6d11af35e4205656919e278a24029ddd00ddff3b"),
    (_PORTS + "pool/main/x/xz-utils/liblzma-dev_5.2.4-1ubuntu1.1_arm64.deb",
     "2feba24ec96e502cce0c2df217bd92d4739929ce6af2b63d5eca3f83b21ec05b"),
    # libcurl needs: nghttp2, idn2, rtmp, ssh, psl, zstd, brotli, ldap
    (_PORTS + "pool/main/n/nghttp2/libnghttp2-dev_1.40.0-1ubuntu0.3_arm64.deb",
     "0e2e86dfb67ad02b2b04e6c0d7ec3ef241234d1b8b5a510a0cd706e81dfc2f2f"),
    (_PORTS + "pool/main/libi/libidn2/libidn2-dev_2.2.0-2_arm64.deb",
     "7b9434d73d444397035429f5ffed52c15dbc231e7763ca0373de55682ed188ed"),
    (_PORTS + "pool/main/r/rtmpdump/librtmp-dev_2.4+20151223.gitfa8646d.1-2build1_arm64.deb",
     "072c77b014aa00e113bbc5b2267b4591725e8a112be709c7c91a27aa95ace881"),
    (_PORTS + "pool/main/libs/libssh/libssh-dev_0.9.3-2ubuntu2.5_arm64.deb",
     "962777552bfb4a2ba3a49b80dda3ab4722a809ffd26f18e665190acb2554ed0c"),
    (_PORTS + "pool/main/libp/libpsl/libpsl-dev_0.21.0-1ubuntu1_arm64.deb",
     "bde0df0d68cf032df80e69bbad2dbf8d5ec0e2821bd6a6d4055e097e1695a5a9"),
    (_PORTS + "pool/main/libz/libzstd/libzstd-dev_1.4.4+dfsg-3ubuntu0.1_arm64.deb",
     "b163aebc48f52ec5f23122f85905b28ff7bc017c6e5b0e271777e17db6fe2007"),
    (_PORTS + "pool/main/b/brotli/libbrotli-dev_1.0.7-6ubuntu0.1_arm64.deb",
     "548f7c052e4a77d3bcd17b37fbc29d8ba97c519b2f86440b7d804aa9d8f4a28f"),
    # Focal uses openldap 2.4 (libldap2-dev, not libldap-dev as in Jammy)
    (_PORTS + "pool/main/o/openldap/libldap2-dev_2.4.49+dfsg-2ubuntu1.10_arm64.deb",
     "8214d6c8e8b4c64d92158c652f207816edd62ec1cf827889edef39ca0765cd3f"),
    # libmicrohttpd needs gnutls and its deps: gmp, nettle/hogweed, tasn1, unistring, p11-kit
    (_PORTS + "pool/main/g/gnutls28/libgnutls28-dev_3.6.13-2ubuntu1.12_arm64.deb",
     "a37b2f9a0dd4ebfc497eee9b3e0847aeadd5c5e4327f508be096ac0b8577c056"),
    (_PORTS + "pool/main/g/gmp/libgmp-dev_6.2.0+dfsg-4ubuntu0.1_arm64.deb",
     "c508e030a0b26557c3d951953df9fe3cc2247386fcd04f4ae81b2fb9f7561fba"),
    (_PORTS + "pool/main/n/nettle/nettle-dev_3.5.1+really3.5.1-2ubuntu0.2_arm64.deb",
     "6d960fa201ce74cef58de75817b42d89b2e844c54a89e6511ca2440aef7c1e68"),
    (_PORTS + "pool/main/libt/libtasn1-6/libtasn1-6-dev_4.16.0-2ubuntu0.1_arm64.deb",
     "713ba67e1e7ed8993e5e8a2bc64e640a24c58c172c261071c4dc30800a726a7b"),
    (_PORTS + "pool/main/libu/libunistring/libunistring-dev_0.9.10-2_arm64.deb",
     "a9af0e7e8fd06f7e2e45ad617ac515b1d2ae2f8a1290ce3522686a89a3f9e648"),
    (_PORTS + "pool/main/p/p11-kit/libp11-kit-dev_0.23.20-1ubuntu0.1_arm64.deb",
     "25eeb936316bc0ce61e95afb1461f7ff18af4e74d707203aacd94a4bdf50f8d5"),
    # libpq needs pgcommon + pgport (not bundled in libpq.a on arm64)
    (_PORTS + "pool/universe/p/postgresql-12/postgresql-server-dev-12_12.22-0ubuntu0.20.04.4_arm64.deb",
     "7a70b2674718a13200dcd00c66d8bdffd23da37e625f0b379ff7de9a5414630b"),
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
#     They install to usr/lib/postgresql/12/lib/ — copy into standard lib dir.
if [ -d "$TMP/usr/lib/postgresql/12/lib" ]; then
    mkdir -p "$SYSROOT/usr/lib/aarch64-linux-gnu"
    for a in "$TMP/usr/lib/postgresql/12/lib/"*.a; do
        [ -f "$a" ] && cp -a "$a" "$SYSROOT/usr/lib/aarch64-linux-gnu/"
    done
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
        target_libc            = "glibc_2.31",
        compiler               = "clang",
        abi_version            = "aarch64",
        abi_libc_version       = "glibc_2.31",
        tool_paths             = tool_paths,
        features               = [default_flags, link_flags, supports_pic, supports_dynamic_linker],
        # Tell Bazel where system headers live so it tracks them as inputs.
        cxx_builtin_include_directories = [
            sysroot + "/usr/include",
            sysroot + "/usr/include/c++/9",
            sysroot + "/usr/include/c++/9/backward",
            sysroot + "/usr/include/aarch64-linux-gnu",
            sysroot + "/usr/include/c++/9/aarch64-linux-gnu",
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
