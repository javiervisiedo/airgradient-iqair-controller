// fw_version.h: Single source of truth for firmware version (SemVer + Git build metadata).
#pragma once

#include <stdint.h>

#define FW_VERSION_MAJOR 0
#define FW_VERSION_MINOR 9
#define FW_VERSION_PATCH 0
#define FW_VERSION_STR "0.9.0"
#define FW_VERSION_CODE ((FW_VERSION_MAJOR * 10000u) + (FW_VERSION_MINOR * 100u) + FW_VERSION_PATCH)

#ifndef FW_GIT_SHA
#define FW_GIT_SHA "dev"
#endif

constexpr uint16_t kFwVersionMajor = FW_VERSION_MAJOR;
constexpr uint16_t kFwVersionMinor = FW_VERSION_MINOR;
constexpr uint16_t kFwVersionPatch = FW_VERSION_PATCH;
constexpr uint32_t kFwVersionCode = FW_VERSION_CODE;
constexpr const char* kFwVersion = FW_VERSION_STR;
constexpr const char* kFwGitSha = FW_GIT_SHA;
