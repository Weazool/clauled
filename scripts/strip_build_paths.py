# Keep the build machine's absolute paths out of the firmware image.
#
# ESP-IDF's logging macros bake __FILE__ into the binary, and for framework
# sources that is an absolute path - so a firmware.bin attached to a public
# release published the developer's home directory, and with it their OS
# username. Verified: 11 such strings before this script existed.
#
# This cannot be done with a plain -ffile-prefix-map in build_flags, for two
# reasons, both of which cost a build to discover:
#
#   1. ${platformio.packages_dir} interpolates with BACKSLASHES on Windows,
#      while the paths actually embedded use forward slashes. GCC matches the
#      prefix literally, so the backslash form silently never matches.
#   2. Hardcoding the forward-slash form would put the username into
#      platformio.ini, which is tracked - trading a leak in the binary for a
#      worse one in the source.
#
# Computing it here fixes both: the path is derived on whatever machine is
# building, and nothing machine-specific is ever written down.

Import("env")

for var, replacement in (
    ("$PROJECT_PACKAGES_DIR", "/pkg"),
    ("$PROJECT_WORKSPACE_DIR", "/build"),
    ("$PROJECT_DIR", "/src"),
):
    path = env.subst(var).replace("\\", "/")
    if path:
        # Both spellings: the toolchain sees forward slashes in __FILE__, but
        # a native Windows path can reach it through other routes.
        env.Append(CCFLAGS=["-ffile-prefix-map=%s=%s" % (path, replacement)])
        env.Append(CXXFLAGS=["-ffile-prefix-map=%s=%s" % (path, replacement)])
        back = path.replace("/", "\\")
        if back != path:
            env.Append(CCFLAGS=["-ffile-prefix-map=%s=%s" % (back, replacement)])
            env.Append(CXXFLAGS=["-ffile-prefix-map=%s=%s" % (back, replacement)])
