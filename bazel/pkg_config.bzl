"""Repository rule that wraps a pkg-config system library.

Usage in WORKSPACE:
    load("//bazel:pkg_config.bzl", "pkg_config_library")
    pkg_config_library(name = "libxml2", pkg = "libxml-2.0")

This creates an external repository @libxml2 with a single cc_library
target //:libxml2.  Uses --static to get all transitive deps, and
resolves -lXXX to full .a paths where available for static linking.
"""

# Directories to search for static .a files on the x86_64 host.
_STATIC_LIB_DIRS = [
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib/postgresql/16/lib",  # pgcommon, pgport for libpq static (Ubuntu 24.04)
    "/usr/lib/postgresql/14/lib",  # pgcommon, pgport for libpq static (Ubuntu 22.04)
    "/usr/lib",
]

# Minimal Cyrus-SASL stub compiled from source at workspace-setup time.
# Ubuntu's libsasl2.a compiles in Berkeley DB and MySQL backends which
# drag in libdb/libmariadb.  We only need the symbols libcurl references
# at link time; SASL auth is never exercised by this application.
_SASL_STUB_SRC = """\
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
"""

def _compile_sasl_stub(rctx):
    """Compile libsasl2.a from source into this repository's output directory."""
    rctx.file("_sasl_stub.c", content = _SASL_STUB_SRC)
    r = rctx.execute(["gcc", "-c", "-O2", "_sasl_stub.c", "-o", "_sasl_stub.o"])
    if r.return_code != 0:
        fail("SASL stub compile failed:\n" + r.stderr)
    r = rctx.execute(["ar", "rcs", "libsasl2.a", "_sasl_stub.o"])
    if r.return_code != 0:
        fail("SASL stub ar failed:\n" + r.stderr)
    rctx.execute(["rm", "-f", "_sasl_stub.c", "_sasl_stub.o"])

def _pkg_config_impl(rctx):
    pkg = rctx.attr.pkg
    name = rctx.name
    extra_linkopts = rctx.attr.extra_linkopts

    # Compile the SASL stub into this repo's output dir so -lsasl2 resolves
    # to our minimal stub rather than the system libsasl2.a (which pulls in
    # Berkeley DB and MySQL transitive deps we don't need).
    _compile_sasl_stub(rctx)
    static_lib_dirs = [str(rctx.path("."))] + _STATIC_LIB_DIRS

    cflags_res = rctx.execute(["pkg-config", "--cflags", pkg])
    libs_res   = rctx.execute(["pkg-config", "--libs", "--static", pkg])

    if cflags_res.return_code != 0:
        fail("pkg-config --cflags {} failed:\n{}".format(pkg, cflags_res.stderr))
    if libs_res.return_code != 0:
        fail("pkg-config --libs --static {} failed:\n{}".format(pkg, libs_res.stderr))

    # Parse -I flags; symlink each include directory into the repository.
    includes = []
    for flag in [f for f in cflags_res.stdout.strip().split(" ") if f]:
        if flag.startswith("-I"):
            abs_path  = flag[2:]
            local     = "include" + abs_path.replace("/", "_")
            rctx.symlink(abs_path, local)
            includes.append(local)

    # Libraries that must remain dynamic (glibc internals, or .a has complex
    # transitive dep chains that are impractical to fully vendor statically).
    _ALWAYS_DYNAMIC = ["m", "c", "dl", "pthread", "rt", "resolv", "nsl", "util",
                       "ldap", "lber"]

    # Parse lib flags: resolve -lXXX to full .a paths where possible.
    # This gives static linking without needing -Wl,-Bstatic.
    linkopts = []
    seen = {}  # deduplicate
    for flag in [f for f in libs_res.stdout.strip().split(" ") if f]:
        if flag in seen:
            continue
        seen[flag] = True
        if flag.startswith("-l"):
            libname = flag[2:]
            found = None
            if libname not in _ALWAYS_DYNAMIC:
                for d in static_lib_dirs:
                    r = rctx.execute(["test", "-f", d + "/lib" + libname + ".a"])
                    if r.return_code == 0:
                        found = d + "/lib" + libname + ".a"
                        break
            if found:
                linkopts.append(found)
            else:
                linkopts.append(flag)  # no .a available, keep dynamic
        elif flag.startswith("-L"):
            pass  # skip -L flags; we use full paths for static .a files
        else:
            linkopts.append(flag)

    # Append extra transitive linkopts (resolved to .a paths where available).
    for flag in extra_linkopts:
        if flag in seen:
            continue
        seen[flag] = True
        if flag.startswith("-l"):
            libname = flag[2:]
            found = None
            if libname not in _ALWAYS_DYNAMIC:
                for d in static_lib_dirs:
                    r = rctx.execute(["test", "-f", d + "/lib" + libname + ".a"])
                    if r.return_code == 0:
                        found = d + "/lib" + libname + ".a"
                        break
            linkopts.append(found if found else flag)
        else:
            linkopts.append(flag)

    # Wrap the archives in --start-group/--end-group so the linker makes
    # multiple passes over the group and resolves circular/forward references
    # (e.g. libhogweed → libgmp where pkg-config places gmp before hogweed).
    final_linkopts = ["-Wl,--start-group"] + linkopts + ["-Wl,--end-group"]

    # Build the hdrs glob expression.
    if includes:
        glob_patterns = (
            ['"{}/**/*.h"'.format(d) for d in includes] +
            ['"{}/**/*.hpp"'.format(d) for d in includes]
        )
        hdrs_expr = "glob([{}])".format(", ".join(glob_patterns))
    else:
        hdrs_expr = "[]"

    rctx.file("BUILD.bazel", content = """\
cc_library(
    name = "{name}",
    hdrs = {hdrs},
    includes = {includes},
    linkopts = {linkopts},
    visibility = ["//visibility:public"],
)
""".format(
        name     = name,
        hdrs     = hdrs_expr,
        includes = str(includes),
        linkopts = str(final_linkopts),
    ))

pkg_config_library = repository_rule(
    implementation = _pkg_config_impl,
    attrs = {
        "pkg": attr.string(mandatory = True, doc = "pkg-config package name"),
        "extra_linkopts": attr.string_list(
            default = [],
            doc = "Extra -l flags for transitive deps not reported by pkg-config --static.",
        ),
    },
    local = True,
    doc = "Creates a cc_library target wrapping a system package via pkg-config.",
)
