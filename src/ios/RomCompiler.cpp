#include "RomCompiler.h"

#include <array>

#include "arm_decode.h"
#include "thumb_decode.h"

namespace {

constexpr unsigned kCacheBits = 17;
std::array<armv4t::Instr, 1u << kCacheBits> decoded;

uint32_t read32(const uint8_t* bytes) {
    return uint32_t{bytes[0]} | uint32_t{bytes[1]} << 8 |
           uint32_t{bytes[2]} << 16 | uint32_t{bytes[3]} << 24;
}

} // namespace

armv4t::Instr sky_decode_instruction(uint32_t pc, uint32_t raw, bool thumb) {
    const uint32_t key = pc | static_cast<uint32_t>(thumb);
    const uint32_t index = (key * 0x9e3779b1u) >> (32 - kCacheBits);
    auto& instruction = decoded[index];
    // Always match the actual bus fetch. Overlays can replace code at the
    // same address and the guest can copy, decompress or rewrite it in RAM.
    if (instruction.pc != pc || instruction.raw != raw ||
        instruction.thumb != thumb) {
        instruction = thumb ? armv4t::ThumbDecoder::decode(
                                  static_cast<uint16_t>(raw), pc)
                            : armv4t::ArmDecoder::decode(raw, pc);
    }
    return instruction;
}

std::size_t sky_compile_rom(const uint8_t* rom, std::size_t size) {
    if (size < 0x40) return 0;
    // Force every slot to miss the first lookup, including ARM word 0 at PC 0.
    for (auto& instruction : decoded) instruction.pc = UINT32_MAX;

    std::size_t count = 0;
    for (unsigned header : {0x20u, 0x30u}) {
        const uint32_t offset = read32(rom + header);
        const uint32_t pc = read32(rom + header + 8);
        const uint32_t length = read32(rom + header + 12);
        if (offset > size || length > size - offset ||
            length > UINT32_MAX - pc || (pc & 3u) != 0) {
            return 0;
        }
        for (uint32_t i = 0; i < (length & ~3u); i += 4u) {
            const uint32_t raw = read32(rom + offset + i);
            sky_decode_instruction(pc + i, raw, false);
            sky_decode_instruction(pc + i, raw & 0xffffu, true);
            sky_decode_instruction(pc + i + 2u, raw >> 16, true);
            count += 3;
        }
    }
    return count;
}
