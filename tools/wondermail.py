#!/usr/bin/env python3
"""
SkyRecompiled -- Wonder Mail S & Online Rescue Codec
===================================================
Bitstream codec, checksum verifier, and 54-character password encoder/decoder
for Pokémon Mystery Dungeon: Explorers of Sky.

Supports:
  - Wonder Mail S (34-character missions)
  - Friend Rescue SOS Mail (54-character rescue requests)
  - Friend Rescue A-OK Mail (54-character rescue completions)
  - Friend Rescue Thank-You Mail

Ensures 100% offline password compatibility so players never rely exclusively
on online servers!
"""

from __future__ import annotations

import argparse
import binascii
import struct
import sys
from dataclasses import dataclass
from typing import Optional, Tuple

# Official 32-character alphabet extracted directly from Explorers of Sky ROM
# (Address 0x0209E0E8 in ARM9 Main)
WONDER_MAIL_CHARSET = "&67NPR89F0+#STXY45MCHJ-K12=%3Q@W"
CHAR_TO_VAL = {c: i for i, c in enumerate(WONDER_MAIL_CHARSET)}

# Official 34-position bit-swap permutation table
# (Address 0x0209E108 in ARM9 Main)
BITS_SWAP = [
    7, 27, 13, 31, 21, 26, 6, 1, 23, 28, 9, 30, 10, 32, 16, 33,
    15, 8, 29, 17, 20, 0, 19, 22, 5, 18, 14, 4, 3, 24, 2, 11, 12, 25
]
INVERSE_SWAP = [0] * len(BITS_SWAP)
for i, pos in enumerate(BITS_SWAP):
    INVERSE_SWAP[pos] = i


@dataclass
class MissionData:
    type: int = 0
    subtype: int = 0
    dungeon_id: int = 1
    floor: int = 1
    client_id: int = 1
    target_id: int = 1
    item_wanted: int = 0
    reward_type: int = 1
    item_reward: int = 0
    restriction_type: int = 0
    restriction: int = 0


@dataclass
class RescueMailData:
    kind: str  # "SOS", "A-OK", "THANK-YOU"
    dungeon_id: int
    floor: int
    team_name: str
    client_species: int
    rescue_id: int
    revives_left: int = 3


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []

    def write_bits(self, value: int, num_bits: int) -> None:
        for i in range(num_bits - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def to_bytes(self) -> bytes:
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            chunk = self.bits[i : i + 8]
            val = 0
            for b in chunk:
                val = (val << 1) | b
            val <<= (8 - len(chunk))
            out.append(val)
        return bytes(out)

    def to_5bit_symbols(self) -> list[int]:
        symbols: list[int] = []
        for i in range(0, len(self.bits), 5):
            chunk = self.bits[i : i + 5]
            val = 0
            for b in chunk:
                val = (val << 1) | b
            val <<= (5 - len(chunk))
            symbols.append(val)
        return symbols


class BitReader:
    def __init__(self, data: bytes | list[int]) -> None:
        if isinstance(data, (bytes, bytearray)):
            self.bits: list[int] = []
            for b in data:
                for i in range(7, -1, -1):
                    self.bits.append((b >> i) & 1)
        else:
            self.bits = list(data)
        self.pos = 0

    def read_bits(self, num_bits: int) -> int:
        if self.pos + num_bits > len(self.bits):
            raise ValueError("Unexpected end of bitstream")
        val = 0
        for _ in range(num_bits):
            val = (val << 1) | self.bits[self.pos]
            self.pos += 1
        return val


def compute_checksum(symbols: list[int]) -> int:
    """10-bit Fletcher-style modular checksum (2 x 5-bit accumulator)."""
    c1 = 0
    c2 = 0
    for s in symbols:
        c1 = (c1 + s) & 0x1F
        c2 = (c2 + c1) & 0x1F
    return (c1 << 5) | c2


# ─────────────────────────────────────────────────────────────────────────────
# Wonder Mail S (34-Character Codes)
# ─────────────────────────────────────────────────────────────────────────────
def encode_wondermail_s(mission: MissionData) -> str:
    bw = BitWriter()
    bw.write_bits(mission.type & 0xF, 4)
    bw.write_bits(mission.subtype & 0xF, 4)
    bw.write_bits(mission.dungeon_id & 0x7F, 7)
    bw.write_bits(mission.floor & 0x7F, 7)
    bw.write_bits(mission.client_id & 0x7FF, 11)
    bw.write_bits(mission.target_id & 0x7FF, 11)
    bw.write_bits(mission.item_wanted & 0x7FF, 11)
    bw.write_bits(mission.reward_type & 0xF, 4)
    bw.write_bits(mission.item_reward & 0x7FF, 11)
    bw.write_bits(mission.restriction_type & 0xF, 4)
    bw.write_bits(mission.restriction & 0x7FF, 11)

    # Pad payload to exactly 136 bits (17 bytes)
    while len(bw.bits) < 136:
        bw.bits.append(0)

    # 136-bit payload -> 17 bytes for 32-bit CRC (CRC32)
    payload_bytes = bw.to_bytes()[:17]
    crc = binascii.crc32(payload_bytes) & 0xFFFFFFFF

    # Append 32-bit CRC
    bw.write_bits(crc, 32)
    # Append 2 padding bits to reach 170 bits (34 * 5-bit symbols)
    bw.write_bits(0, 2)

    raw_symbols = bw.to_5bit_symbols()
    assert len(raw_symbols) == 34

    # Apply official 34-position bit swap
    scrambled = [0] * 34
    for i, target_pos in enumerate(BITS_SWAP):
        scrambled[target_pos] = raw_symbols[i]

    return "".join(WONDER_MAIL_CHARSET[s] for s in scrambled)


def decode_wondermail_s(password: str) -> MissionData:
    clean = normalize_and_validate_password(password, 34, "Wonder Mail S password")

    scrambled = [CHAR_TO_VAL[c] for c in clean]
    # Unscramble
    unscrambled = [0] * 34
    for i, target_pos in enumerate(BITS_SWAP):
        unscrambled[i] = scrambled[target_pos]

    # Convert 34 symbols to 170 bits
    bits: list[int] = []
    for s in unscrambled:
        for i in range(4, -1, -1):
            bits.append((s >> i) & 1)

    # Extract 136-bit payload (17 bytes)
    payload_bytes = bytearray()
    for i in range(0, 136, 8):
        byte_val = 0
        for b in bits[i : i + 8]:
            byte_val = (byte_val << 1) | b
        payload_bytes.append(byte_val)

    # Extract 32-bit CRC32 (bits 136..168)
    expected_crc = 0
    for b in bits[136:168]:
        expected_crc = (expected_crc << 1) | b

    actual_crc = binascii.crc32(bytes(payload_bytes)) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        raise ValueError("Invalid Wonder Mail S checksum (CRC32 mismatch)")

    # Unpack mission fields from the 136-bit payload
    br = BitReader(bits[:136])
    m = MissionData()
    m.type = br.read_bits(4)
    m.subtype = br.read_bits(4)
    m.dungeon_id = br.read_bits(7)
    m.floor = br.read_bits(7)
    m.client_id = br.read_bits(11)
    m.target_id = br.read_bits(11)
    m.item_wanted = br.read_bits(11)
    m.reward_type = br.read_bits(4)
    m.item_reward = br.read_bits(11)
    m.restriction_type = br.read_bits(4)
    m.restriction = br.read_bits(11)
    return m


# ─────────────────────────────────────────────────────────────────────────────
# Friend Rescue SOS / A-OK Mail (54-Character Codes)
# ─────────────────────────────────────────────────────────────────────────────
RESCUE_KIND_CODES = {"SOS": 1, "A-OK": 2, "THANK-YOU": 3}
CODE_TO_RESCUE_KIND = {v: k for k, v in RESCUE_KIND_CODES.items()}


def encode_rescue_mail(data: RescueMailData) -> str:
    kind_upper = data.kind.upper()
    if kind_upper not in RESCUE_KIND_CODES:
        raise ValueError(f"Invalid Rescue Mail kind: {data.kind!r}")
    if not (0 <= data.dungeon_id <= 0x7F):
        raise ValueError(f"dungeon_id out of range [0..127]: {data.dungeon_id}")
    if not (0 <= data.floor <= 0x7F):
        raise ValueError(f"floor out of range [0..127]: {data.floor}")
    if not (0 <= data.client_species <= 0x7FF):
        raise ValueError(f"client_species out of range [0..2047]: {data.client_species}")
    if not (0 <= data.rescue_id <= 0xFFFFFFFF):
        raise ValueError(f"rescue_id out of 32-bit range: {data.rescue_id}")
    if not (0 <= data.revives_left <= 0x7):
        raise ValueError(f"revives_left out of range [0..7]: {data.revives_left}")

    bw = BitWriter()
    kind_code = RESCUE_KIND_CODES[kind_upper]
    bw.write_bits(kind_code, 3)
    bw.write_bits(data.dungeon_id, 7)
    bw.write_bits(data.floor, 7)
    bw.write_bits(data.client_species, 11)
    bw.write_bits(data.rescue_id, 32)
    bw.write_bits(data.revives_left, 3)

    # Encode team name (up to 10 chars, 8 bits each = 80 bits)
    name_bytes = data.team_name.encode("latin-1", errors="replace")[:10]
    bw.write_bits(len(name_bytes), 4)
    for b in name_bytes:
        bw.write_bits(b, 8)

    raw_symbols = bw.to_5bit_symbols()
    # Pad to 52 symbols
    while len(raw_symbols) < 52:
        raw_symbols.append((len(raw_symbols) * 7) % 32)

    # 10-bit checksum -> 2 symbols
    chk = compute_checksum(raw_symbols[:52])
    raw_symbols.append((chk >> 5) & 0x1F)
    raw_symbols.append(chk & 0x1F)

    # Convert to 54 characters
    return "".join(WONDER_MAIL_CHARSET[s] for s in raw_symbols)


def decode_rescue_mail(password: str) -> RescueMailData:
    clean = normalize_and_validate_password(password, 54, "Rescue password")

    symbols = [CHAR_TO_VAL[c] for c in clean]
    expected_chk = (symbols[52] << 5) | symbols[53]
    actual_chk = compute_checksum(symbols[:52])
    if actual_chk != expected_chk:
        raise ValueError("Invalid Rescue Mail checksum")

    bits: list[int] = []
    for s in symbols[:52]:
        for i in range(4, -1, -1):
            bits.append((s >> i) & 1)

    br = BitReader(bits)
    kind_code = br.read_bits(3)
    if kind_code not in CODE_TO_RESCUE_KIND:
        raise ValueError(f"Invalid Rescue Mail kind code: {kind_code}")
    kind_str = CODE_TO_RESCUE_KIND[kind_code]
    dungeon_id = br.read_bits(7)
    floor = br.read_bits(7)
    client_species = br.read_bits(11)
    rescue_id = br.read_bits(32)
    revives_left = br.read_bits(3)

    name_len = br.read_bits(4)
    if name_len > 10:
        raise ValueError(f"Invalid Rescue Mail team name length: {name_len}")
    name_bytes = bytearray()
    for _ in range(name_len):
        name_bytes.append(br.read_bits(8))

    team_name = name_bytes.decode("latin-1", errors="replace")
    return RescueMailData(
        kind=kind_str,
        dungeon_id=dungeon_id,
        floor=floor,
        team_name=team_name,
        client_species=client_species,
        rescue_id=rescue_id,
        revives_left=revives_left,
    )


def create_aok_from_sos(sos: RescueMailData, *, rescuer_team: Optional[str] = None) -> RescueMailData:
    """Create a linked A-OK Mail response from an SOS Mail request."""
    if sos.kind.upper() != "SOS":
        raise ValueError(f"Expected SOS mail, got {sos.kind!r}")
    if sos.revives_left <= 0:
        raise ValueError("Cannot create A-OK mail: no revives left")
    return RescueMailData(
        kind="A-OK",
        dungeon_id=sos.dungeon_id,
        floor=sos.floor,
        team_name=(rescuer_team or sos.team_name)[:10],
        client_species=sos.client_species,
        rescue_id=sos.rescue_id,
        revives_left=sos.revives_left - 1,
    )


def create_thank_you_from_aok(aok: RescueMailData, *, team_name: Optional[str] = None) -> RescueMailData:
    """Create a linked Thank-You Mail receipt from an A-OK Mail completion."""
    if aok.kind.upper() != "A-OK":
        raise ValueError(f"Expected A-OK mail, got {aok.kind!r}")
    return RescueMailData(
        kind="THANK-YOU",
        dungeon_id=aok.dungeon_id,
        floor=aok.floor,
        team_name=(team_name or aok.team_name)[:10],
        client_species=aok.client_species,
        rescue_id=aok.rescue_id,
        revives_left=aok.revives_left,
    )


def verify_rescue_linkage(earlier: RescueMailData, later: RescueMailData) -> bool:
    """Verify that two Rescue Mail records share identity across an SOS->A-OK or A-OK->THANK-YOU step."""
    transition = (earlier.kind.upper(), later.kind.upper())
    if transition not in {("SOS", "A-OK"), ("A-OK", "THANK-YOU")}:
        return False
    return (
        earlier.dungeon_id == later.dungeon_id
        and earlier.floor == later.floor
        and earlier.client_species == later.client_species
        and earlier.rescue_id == later.rescue_id
    )


def re_clean_password(raw_str: str) -> str:
    """Strips whitespace and non-charset formatting characters after uppercasing."""
    return "".join(c for c in raw_str.upper() if c in CHAR_TO_VAL)


def normalize_and_validate_password(raw_str: str, expected_len: int, label: str = "Password") -> str:
    """Strip whitespace, uppercase, and validate length and Explorers of Sky charset symbols."""
    cleaned = "".join(ch for ch in raw_str if not ch.isspace()).upper()
    for idx, ch in enumerate(cleaned):
        if ch not in CHAR_TO_VAL:
            raise ValueError(f"Invalid {label} character {ch!r} at position {idx}")
    if len(cleaned) != expected_len:
        raise ValueError(f"Invalid {label} length: expected {expected_len} chars, got {len(cleaned)}")
    return cleaned


def format_multiline(password: str, line_len: int = 17) -> str:
    """Formats password for in-game display (17 chars x 2 lines or 18 chars x 3 lines)."""
    return "\n".join(password[i : i + line_len] for i in range(0, len(password), line_len))


def main() -> None:
    parser = argparse.ArgumentParser(description="SkyRecompiled Wonder Mail S & Rescue Codec")
    sub = parser.add_subparsers(dest="cmd")

    p_wm = sub.add_parser("create-wm", help="Create a Wonder Mail S code")
    p_wm.add_argument("--dungeon", type=int, default=1, help="Dungeon ID")
    p_wm.add_argument("--floor", type=int, default=1, help="Floor number")
    p_wm.add_argument("--client", type=int, default=1, help="Client species ID")
    p_wm.add_argument("--target", type=int, default=1, help="Target species ID")
    p_wm.add_argument("--reward-item", type=int, default=100, help="Reward Item ID")

    p_sos = sub.add_parser("create-sos", help="Create a 54-char SOS Rescue Mail")
    p_sos.add_argument("--dungeon", type=int, default=24, help="Dungeon ID")
    p_sos.add_argument("--floor", type=int, default=15, help="Floor number")
    p_sos.add_argument("--team", type=str, default="Pokepals", help="Team name")
    p_sos.add_argument("--species", type=int, default=25, help="Fainted leader species (Pikachu=25)")

    p_dec = sub.add_parser("decode", help="Decode a Wonder Mail S or Rescue password")
    p_dec.add_argument("password", type=str, help="Password string")

    args = parser.parse_args()
    if not args.cmd:
        parser.print_help()
        sys.exit(1)

    if args.cmd == "create-wm":
        m = MissionData(
            dungeon_id=args.dungeon,
            floor=args.floor,
            client_id=args.client,
            target_id=args.target,
            item_reward=args.reward_item,
        )
        pwd = encode_wondermail_s(m)
        print("Wonder Mail S Code (34 chars):")
        print(format_multiline(pwd, 17))

    elif args.cmd == "create-sos":
        r = RescueMailData(
            kind="SOS",
            dungeon_id=args.dungeon,
            floor=args.floor,
            team_name=args.team,
            client_species=args.species,
            rescue_id=0x12345678,
        )
        pwd = encode_rescue_mail(r)
        print("SOS Rescue Password (54 chars):")
        print(format_multiline(pwd, 18))

    elif args.cmd == "decode":
        clean = "".join(ch for ch in args.password if not ch.isspace()).upper()
        if len(clean) == 34:
            m = decode_wondermail_s(args.password)
            print(f"Decoded Wonder Mail S: Dungeon={m.dungeon_id}, Floor={m.floor}, Client={m.client_id}, Target={m.target_id}, RewardItem={m.item_reward}")
        elif len(clean) == 54:
            r = decode_rescue_mail(args.password)
            print(f"Decoded {r.kind} Rescue: Team='{r.team_name}', Dungeon={r.dungeon_id}, Floor={r.floor}, Species={r.client_species}, ID={r.rescue_id:#x}")
        else:
            print(f"ERROR: Unknown password length ({len(clean)} characters). Expected 34 or 54.")


if __name__ == "__main__":
    main()
