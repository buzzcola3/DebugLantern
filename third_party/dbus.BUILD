###############################################################################
# D-Bus 1.14.10 – build just the client library (libdbus-1)
#
# We only need the client-side library so that avahi-client can talk to
# the system avahi-daemon over D-Bus.
###############################################################################
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

configure_make(
    name = "dbus",
    lib_source = ":all_srcs",
    visibility = ["//visibility:public"],
    configure_options = [
        "--disable-tests",
        "--disable-modular-tests",
        "--disable-systemd",
        "--disable-selinux",
        "--disable-apparmor",
        "--disable-launchd",
        "--disable-xml-docs",
        "--disable-doxygen-docs",
        "--disable-ducktype-docs",
        "--disable-verbose-mode",
        "--disable-stats",
        "--without-x",
        # Use expat for XML parsing (provided hermetically via deps)
        "--with-xml=expat",
    ] + select({
        "@platforms//cpu:aarch64": ["--host=aarch64-linux-gnu"],
        "//conditions:default": [],
    }),
    # Tell configure where to find hermetic expat (bypasses pkg-config)
    env = {
        "EXPAT_CFLAGS": "-I$$EXT_BUILD_DEPS$$/include",
        "EXPAT_LIBS": "-L$$EXT_BUILD_DEPS$$/lib -lexpat",
    },
    deps = [
        "@libexpat//:libexpat",
    ],
    out_static_libs = ["libdbus-1.a"],
    out_include_dir = "include/dbus-1.0",
)

