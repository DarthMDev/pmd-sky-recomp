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


if __name__ == "__main__":
    unittest.main()
