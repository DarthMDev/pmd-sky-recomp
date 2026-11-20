#!/usr/bin/env python3
"""
Unit tests for SkyRecompiled Wonder Mail S & Friend Rescue bitstream codec.
Verifies 34-character Wonder Mail S and 54-character SOS/A-OK passwords.
"""

import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.wondermail import (
    MissionData,
    RescueMailData,
    encode_wondermail_s,
    decode_wondermail_s,
    encode_rescue_mail,
    decode_rescue_mail,
    create_aok_from_sos,
    create_thank_you_from_aok,
    verify_rescue_linkage,
    re_clean_password,
    WONDER_MAIL_CHARSET,
)


class TestWonderMailS(unittest.TestCase):
    def test_roundtrip_basic_mission(self):
        mission = MissionData(
            type=1,
            subtype=2,
            dungeon_id=14,  # Apple Woods
            floor=9,
            client_id=25,   # Pikachu
            target_id=133,  # Eevee
            item_wanted=50,
            reward_type=1,
            item_reward=105, # Golden Apple
            restriction_type=0,
            restriction=0,
        )
        pwd = encode_wondermail_s(mission)
        self.assertEqual(len(pwd), 34)
        for char in pwd:
            self.assertIn(char, WONDER_MAIL_CHARSET)

        decoded = decode_wondermail_s(pwd)
        self.assertEqual(decoded.type, mission.type)
        self.assertEqual(decoded.subtype, mission.subtype)
        self.assertEqual(decoded.dungeon_id, mission.dungeon_id)
        self.assertEqual(decoded.floor, mission.floor)
        self.assertEqual(decoded.client_id, mission.client_id)
        self.assertEqual(decoded.target_id, mission.target_id)
        self.assertEqual(decoded.item_wanted, mission.item_wanted)
        self.assertEqual(decoded.item_reward, mission.item_reward)

    def test_roundtrip_edge_boundaries(self):
        # Maximum allowed bits in field limits
        mission = MissionData(
            type=0xF,
            subtype=0xF,
            dungeon_id=0x7F,
            floor=0x7F,
            client_id=0x7FF,
            target_id=0x7FF,
            item_wanted=0x7FF,
            reward_type=0xF,
            item_reward=0x7FF,
            restriction_type=0xF,
            restriction=0x7FF,
        )
        pwd = encode_wondermail_s(mission)
        decoded = decode_wondermail_s(pwd)
        self.assertEqual(decoded.dungeon_id, 0x7F)
        self.assertEqual(decoded.floor, 0x7F)
        self.assertEqual(decoded.client_id, 0x7FF)

    def test_checksum_corruption_detection(self):
        mission = MissionData(dungeon_id=5, floor=3, client_id=1, target_id=4)
        pwd = encode_wondermail_s(mission)
        
        # Tamper with first character
        corrupted_char = 'W' if pwd[0] != 'W' else '&'
        corrupted = corrupted_char + pwd[1:]
        with self.assertRaises(ValueError):
            decode_wondermail_s(corrupted)

    def test_whitespace_and_newline_tolerance(self):
        mission = MissionData(dungeon_id=20, floor=15, client_id=10, target_id=20)
        pwd = encode_wondermail_s(mission)
        formatted = f"{pwd[:17]}\n{pwd[17:]}\n   "
        decoded = decode_wondermail_s(formatted)
        self.assertEqual(decoded.dungeon_id, 20)
        self.assertEqual(decoded.floor, 15)


class TestFriendRescueMail(unittest.TestCase):
    def test_roundtrip_sos_mail(self):
        rescue = RescueMailData(
            kind="SOS",
            dungeon_id=24,   # Temporal Tower
            floor=23,
            team_name="Pokepals",
            client_species=393, # Piplup
            rescue_id=0x9ABCDEF0,
            revives_left=3,
        )
        pwd = encode_rescue_mail(rescue)
        self.assertEqual(len(pwd), 54)
        for char in pwd:
            self.assertIn(char, WONDER_MAIL_CHARSET)

        decoded = decode_rescue_mail(pwd)
        self.assertEqual(decoded.kind, "SOS")
        self.assertEqual(decoded.dungeon_id, 24)
        self.assertEqual(decoded.floor, 23)
        self.assertEqual(decoded.team_name, "Pokepals")
        self.assertEqual(decoded.client_species, 393)
        self.assertEqual(decoded.rescue_id, 0x9ABCDEF0)
        self.assertEqual(decoded.revives_left, 3)

    def test_roundtrip_aok_mail(self):
        aok = RescueMailData(
            kind="A-OK",
            dungeon_id=30,  # Zero Isle North
            floor=40,
            team_name="Rescuers",
            client_species=448, # Lucario
            rescue_id=0x55AA3311,
            revives_left=2,
        )
        pwd = encode_rescue_mail(aok)
        self.assertEqual(len(pwd), 54)
        decoded = decode_rescue_mail(pwd)
        self.assertEqual(decoded.kind, "A-OK")
        self.assertEqual(decoded.dungeon_id, 30)
        self.assertEqual(decoded.floor, 40)
        self.assertEqual(decoded.team_name, "Rescuers")

    def test_invalid_lengths(self):
        with self.assertRaises(ValueError):
            decode_rescue_mail("SHORTPASSWORD")
        with self.assertRaises(ValueError):
            decode_wondermail_s("SHORTPASSWORD")

    def test_roundtrip_thank_you_and_full_rescue_cycle(self):
        sos = RescueMailData(
            kind="SOS",
            dungeon_id=42,
            floor=17,
            team_name="Explorers",
            client_species=133,
            rescue_id=0xA5A55A5A,
            revives_left=3,
        )
        sos_pwd = encode_rescue_mail(sos)
        decoded_sos = decode_rescue_mail(sos_pwd)

        aok = create_aok_from_sos(decoded_sos, rescuer_team="Saviors")
        aok_pwd = encode_rescue_mail(aok)
        decoded_aok = decode_rescue_mail(aok_pwd)
        self.assertEqual(decoded_aok.kind, "A-OK")
        self.assertEqual(decoded_aok.revives_left, 2)
        self.assertTrue(verify_rescue_linkage(decoded_sos, decoded_aok))

        ty = create_thank_you_from_aok(decoded_aok, team_name="Explorers")
        ty_pwd = encode_rescue_mail(ty)
        decoded_ty = decode_rescue_mail(ty_pwd)
        self.assertEqual(decoded_ty.kind, "THANK-YOU")
        self.assertEqual(decoded_ty.rescue_id, sos.rescue_id)
        self.assertEqual(decoded_ty.team_name, "Explorers")
        self.assertTrue(verify_rescue_linkage(decoded_aok, decoded_ty))
        self.assertFalse(verify_rescue_linkage(decoded_sos, decoded_ty))

    def test_rescue_id_full_32bit_width(self):
        for rid in (0x00000000, 0x80000000, 0x9ABCDEF0, 0xFFFFFFFF):
            sos = RescueMailData(
                kind="SOS",
                dungeon_id=1,
                floor=2,
                team_name="WidthTest",
                client_species=25,
                rescue_id=rid,
                revives_left=3,
            )
            pwd = encode_rescue_mail(sos)
            decoded = decode_rescue_mail(pwd)
            self.assertEqual(decoded.rescue_id, rid)

    def test_lowercase_and_hyphen_handling(self):
        sos = RescueMailData(
            kind="SOS",
            dungeon_id=7,
            floor=5,
            team_name="Hyphens",
            client_species=387,
            rescue_id=0x12345678,
            revives_left=3,
        )
        pwd = encode_rescue_mail(sos)
        # Ensure we test with lowercase letters AND verify '-' is preserved if present
        self.assertEqual(re_clean_password(pwd.lower()), pwd)
        decoded = decode_rescue_mail(pwd.lower())
        self.assertEqual(decoded.rescue_id, 0x12345678)

        # Force a team/id payload containing '-' in the encoded password
        found_hyphen = False
        for rid in range(0x100, 0x300):
            cand = encode_rescue_mail(
                RescueMailData("SOS", 7, 5, "Hyphens", 387, rid, 3)
            )
            if "-" in cand:
                found_hyphen = True
                self.assertEqual(decode_rescue_mail(cand.lower()).rescue_id, rid)
                break
        self.assertTrue(found_hyphen)

    def test_rejects_invalid_characters(self):
        mission_pwd = encode_wondermail_s(MissionData(dungeon_id=1, floor=1))
        for bad_ch in ("O", "I", "!"):
            bad_pwd = bad_ch + mission_pwd[1:]
            with self.assertRaisesRegex(ValueError, "Invalid Wonder Mail S password character"):
                decode_wondermail_s(bad_pwd)

        sos_pwd = encode_rescue_mail(
            RescueMailData("SOS", 1, 1, "Team", 25, 0x11223344, 3)
        )
        for bad_ch in ("O", "I", "!"):
            bad_pwd = sos_pwd[:5] + bad_ch + sos_pwd[6:]
            with self.assertRaisesRegex(ValueError, "Invalid Rescue password character"):
                decode_rescue_mail(bad_pwd)

    def test_rejects_invalid_rescue_kind_and_field_bounds(self):
        with self.assertRaisesRegex(ValueError, "Invalid Rescue Mail kind"):
            encode_rescue_mail(RescueMailData("INVALID", 1, 1, "Team", 25, 1, 3))
        with self.assertRaisesRegex(ValueError, "dungeon_id out of range"):
            encode_rescue_mail(RescueMailData("SOS", 200, 1, "Team", 25, 1, 3))
        with self.assertRaisesRegex(ValueError, "rescue_id out of 32-bit range"):
            encode_rescue_mail(RescueMailData("SOS", 1, 1, "Team", 25, 0x1_0000_0000, 3))


if __name__ == "__main__":
    unittest.main()
