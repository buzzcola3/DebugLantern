load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "debuglanternd",
    srcs = [
        "src/common.cpp",
        "src/common.h",
        "src/debuglanternd.cpp",
        "src/webui.cpp",
        "src/webui.h",
    ],
    includes = ["src"],
    copts = ["-std=c++20"],
    linkopts = [
        "-lavahi-client",
        "-lavahi-common",
        "-lpthread",
        "-luuid",
    ],
)

cc_binary(
    name = "debuglanternctl",
    srcs = [
        "src/common.cpp",
        "src/common.h",
        "src/debuglanternctl.cpp",
    ],
    includes = ["src"],
    copts = ["-std=c++20"],
)
