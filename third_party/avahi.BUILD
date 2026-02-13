###############################################################################
# Avahi 0.8 – build avahi-common + avahi-client libraries
#
# We only need the client-side libraries for mDNS service advertisement.
# The avahi-daemon itself is expected on the target system.
###############################################################################
load("@rules_foreign_cc//foreign_cc:defs.bzl", "configure_make")

filegroup(
    name = "all_srcs",
    srcs = glob(["**"]),
)

configure_make(
    name = "avahi",
    lib_source = ":all_srcs",
    visibility = ["//visibility:public"],
    configure_options = [
        "--with-distro=none",
        # Enable only what we need
        "--enable-dbus",
        # Disable everything else
        "--disable-glib",
        "--disable-gobject",
        "--disable-introspection",
        "--disable-qt3",
        "--disable-qt4",
        "--disable-qt5",
        "--disable-gtk",
        "--disable-gtk3",
        "--disable-mono",
        "--disable-monodoc",
        "--disable-python",
        "--disable-autoipd",
        "--disable-libdaemon",
        "--disable-libsystemd",
        "--disable-libevent",
        "--disable-manpages",
        "--disable-xmltoman",
        "--disable-tests",
        "--disable-compat-libdns_sd",
        "--disable-compat-howl",
        "--disable-core-docs",
        "--disable-stack-protector",
        "--disable-gdbm",
        "--with-xml=none",
    ] + select({
        "@platforms//cpu:aarch64": ["--host=aarch64-linux-gnu"],
        "//conditions:default": [],
    }),
    deps = [
        "@dbus//:dbus",
    ],
    out_static_libs = [
        "libavahi-common.a",
        "libavahi-client.a",
    ],
    out_include_dir = "include",
)
