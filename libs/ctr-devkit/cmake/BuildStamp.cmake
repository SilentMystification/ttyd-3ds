# SPDX-License-Identifier: MIT
# Copyright (c) the ctr-devkit authors
# cmake -DOUT=<header> -P BuildStamp.cmake: writes the build time (UTC).
string(TIMESTAMP _now "%Y-%m-%d %H:%M:%S" UTC)
file(WRITE "${OUT}" "#define CTRDK_BUILD_STAMP \"${_now}\"\n")
