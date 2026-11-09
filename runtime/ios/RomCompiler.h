#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "arm_ir.h"

inline constexpr std::string_view kSkyUsSha1 = "5fa96ca8d8dd6405d6cd2bad73ed68bc73a9d152";
inline constexpr std::string_view kSkyEuSha1 = "c838a5adf1ed32d2da8454976e5b1a1aa189c139";

inline bool sky_supported_rom(std::string_view sha1) {
    return sha1 == kSkyUsSha1 || sha1 == kSkyEuSha1;
}

// Prepare portable ARM/Thumb IR for the two executable regions in the NDS
// header. Other code, such as relocated overlays, is compiled when fetched.
std::size_t sky_compile_rom(const uint8_t* rom, std::size_t size);
armv4t::Instr sky_decode_instruction(uint32_t pc, uint32_t raw, bool thumb);
