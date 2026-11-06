#include "RomCompiler.h"

#include <array>
#include <cstdio>

namespace {

void write32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
}

bool check(bool ok, const char* message) {
    if (!ok) std::fprintf(stderr, "%s\n", message);
    return ok;
}

} // namespace

int main() {
    std::array<uint8_t, 0x48> rom{};
    write32(rom.data() + 0x20, 0x40);
    write32(rom.data() + 0x28, 0x02000000);
    write32(rom.data() + 0x2c, 4);
    write32(rom.data() + 0x30, 0x44);
    write32(rom.data() + 0x38, 0x02380000);
    write32(rom.data() + 0x3c, 4);
    write32(rom.data() + 0x40, 0xea000000); // ARM B to PC + 8.
    write32(rom.data() + 0x44, 0xe000e000); // Two Thumb branches.

    bool ok = check(sky_compile_rom(rom.data(), rom.size()) == 6,
                    "expected ARM and Thumb candidates for both executable regions");
    ok &= check(sky_decode_instruction(0x02000000, 0xea000000, false).branch_target == 0x02000008,
                "ARM branch target must use the guest address");
    ok &= check(sky_decode_instruction(0x02000000, 0xea000001, false).branch_target == 0x0200000c,
                "an overlay at the same address must use the replacement bytes");
    ok &= check(sky_decode_instruction(0x02380002, 0xe000, true).branch_target == 0x02380006,
                "Thumb branch target must use the aligned halfword address");
    ok &= check(sky_decode_instruction(0x02000000, 0xe000, true).thumb,
                "a mode switch at the same address must decode as Thumb");
    ok &= check(sky_compile_rom(rom.data(), rom.size() - 1) == 0,
                "a truncated executable region must fail preparation");
    return ok ? 0 : 1;
}
