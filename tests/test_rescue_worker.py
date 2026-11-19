#!/usr/bin/env python3
"""
Integration & parity tests for the Cloudflare Rescue Worker (services/rescue-worker/src/index.ts)
and the Python Wonder Mail / Rescue codec (tools/wondermail.py).
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.wondermail import (
    RescueMailData,
    create_aok_from_sos,
    create_thank_you_from_aok,
    encode_rescue_mail,
)

WORKER_SRC = REPO_ROOT / "services" / "rescue-worker" / "src" / "index.ts"

MOCK_D1_JS = r"""
class MockPreparedStatement {
  constructor(db, sql) {
    this.db = db;
    this.sql = sql.replace(/\s+/g, " ").trim();
    this.args = [];
  }
  bind(...args) {
    this.args = args;
    return this;
  }
  async first() {
    return this.db._first(this.sql, this.args);
  }
  async all() {
    return { results: this.db._all(this.sql, this.args) };
  }
  async run() {
    return { meta: { changes: this.db._run(this.sql, this.args) } };
  }
}

class MockD1Database {
  constructor() {
    this.rows = [];
  }
  prepare(sql) {
    return new MockPreparedStatement(this, sql);
  }
  _first(sql, args) {
    if (sql.startsWith("SELECT id FROM rescues WHERE status IN ('open', 'claimed')")) {
      const [rescueId, sosPwd] = args;
      const hit = this.rows.find(
        (r) =>
          (r.status === "open" || r.status === "claimed") &&
          (r.rescue_id === rescueId || r.sos_password === sosPwd)
      );
      return hit ? { ...hit } : null;
    }
    if (sql.includes("FROM rescues WHERE id = ?")) {
      const [id] = args;
      const hit = this.rows.find((r) => r.id === id);
      return hit ? { ...hit } : null;
    }
    throw new Error("Unsupported _first SQL: " + sql);
  }
  _all(sql, args) {
    if (sql.startsWith("SELECT * FROM rescues WHERE status = ?")) {
      let idx = 0;
      const status = args[idx++];
      let filtered = this.rows.filter((r) => r.status === status);
      if (sql.includes("AND dungeon_id = ?")) {
        const did = args[idx++];
        filtered = filtered.filter((r) => r.dungeon_id === did);
      }
      const limit = args[idx++];
      const offset = args[idx++];
      filtered = [...filtered].sort((a, b) => b.created_at - a.created_at);
      return filtered.slice(offset, offset + limit).map((r) => ({ ...r }));
    }
    throw new Error("Unsupported _all SQL: " + sql);
  }
  _run(sql, args) {
    if (sql.startsWith("INSERT INTO rescues")) {
      const [
        id,
        rescue_id,
        dungeon_id,
        floor,
        team_name,
        client_species,
        sos_password,
        friend_code,
        owner_token,
        created_at,
        updated_at,
      ] = args;
      const conflict = this.rows.find(
        (r) =>
          (r.status === "open" || r.status === "claimed") &&
          (r.rescue_id === rescue_id || r.sos_password === sos_password)
      );
      if (conflict) {
        throw new Error("UNIQUE constraint failed: rescues.rescue_id");
      }
      this.rows.push({
        id,
        rescue_id,
        dungeon_id,
        floor,
        team_name,
        client_species,
        sos_password,
        aok_password: null,
        thank_you_password: null,
        status: "open",
        rescuer_team_name: null,
        friend_code,
        owner_token,
        claim_token: null,
        claim_expires_at: null,
        created_at,
        updated_at,
      });
      return 1;
    }
    if (sql.startsWith("UPDATE rescues SET status = 'open', claim_expires_at = NULL")) {
      const [updatedAt, now] = args;
      let changes = 0;
      for (const r of this.rows) {
        if (r.status === "claimed" && r.claim_expires_at !== null && r.claim_expires_at < now) {
          r.status = "open";
          r.claim_expires_at = null;
          r.rescuer_team_name = null;
          r.claim_token = null;
          r.updated_at = updatedAt;
          changes++;
        }
      }
      return changes;
    }
    if (sql.startsWith("UPDATE rescues SET status = 'claimed'")) {
      const [rescuerTeam, claimToken, claimExpires, updatedAt, id, now] = args;
      const r = this.rows.find(
        (row) =>
          row.id === id &&
          (row.status === "open" || (row.status === "claimed" && row.claim_expires_at !== null && row.claim_expires_at < now))
      );
      if (!r) return 0;
      r.status = "claimed";
      r.rescuer_team_name = rescuerTeam;
      r.claim_token = claimToken;
      r.claim_expires_at = claimExpires;
      r.updated_at = updatedAt;
      return 1;
    }
    if (sql.startsWith("UPDATE rescues SET status = 'completed'")) {
      const [aokPwd, updatedAt, id, now, claimToken] = args;
      const r = this.rows.find(
        (row) =>
          row.id === id &&
          (row.status === "open" ||
            (row.status === "claimed" && row.claim_expires_at !== null && row.claim_expires_at < now) ||
            (row.status === "claimed" && claimToken !== null && claimToken !== undefined && row.claim_token === claimToken))
      );
      if (!r) return 0;
      r.status = "completed";
      r.aok_password = aokPwd;
      r.updated_at = updatedAt;
      return 1;
    }
    if (sql.startsWith("UPDATE rescues SET thank_you_password = ?")) {
      const [tyPwd, updatedAt, id] = args;
      const r = this.rows.find(
        (row) => row.id === id && row.status === "completed" && row.thank_you_password === null
      );
      if (!r) return 0;
      r.thank_you_password = tyPwd;
      r.updated_at = updatedAt;
      return 1;
    }
    throw new Error("Unsupported _run SQL: " + sql);
  }
}
"""


def _node_ts_args() -> list[str] | None:
    node = shutil.which("node")
    if not node:
        return None
    res = subprocess.run([node, "--version"], capture_output=True, text=True, check=False)
    if res.returncode != 0:
        return None
    ver = res.stdout.strip().lstrip("v").split(".")
    try:
        major, minor = int(ver[0]), int(ver[1])
    except (ValueError, IndexError):
        return None
    if (major, minor) < (22, 6):
        return None
    if (major, minor) < (23, 6):
        return ["--experimental-strip-types"]
    return []


def _run_node_script(script_body: str) -> dict:
    node = shutil.which("node")
    ts_args = _node_ts_args()
    assert node is not None and ts_args is not None
    with tempfile.TemporaryDirectory() as td:
        script_path = Path(td) / "driver.mjs"
        script_path.write_text(script_body, encoding="utf-8")
        res = subprocess.run(
            [node, *ts_args, str(script_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        if res.returncode != 0:
            raise AssertionError(
                f"Node worker harness failed ({res.returncode}):\nSTDOUT:\n{res.stdout}\nSTDERR:\n{res.stderr}"
            )
        return json.loads(res.stdout)


@unittest.skipUnless(_node_ts_args() is not None, "Node.js >= 22.6 required to execute rescue-worker TypeScript tests")
class TestRescueWorker(unittest.TestCase):
    def test_parse_rescue_mail_parity_with_32bit_msb_ids(self) -> None:
        cases = []
        for idx, rid in enumerate((0x00000000, 0x80000000, 0x9ABCDEF0, 0xFFFFFFFF)):
            kind = ("SOS", "A-OK", "THANK-YOU")[idx % 3]
            mail = RescueMailData(
                kind=kind,
                dungeon_id=24 + idx,
                floor=10 + idx,
                team_name=f"Team{idx}",
                client_species=25 + idx,
                rescue_id=rid,
                revives_left=3 - (idx % 3),
            )
            cases.append((idx % 3 + 1, mail, encode_rescue_mail(mail)))

        passwords = [pwd for _, _, pwd in cases]
        out = _run_node_script(f"""
import {{ pathToFileURL }} from "node:url";
const mod = await import(pathToFileURL({json.dumps(str(WORKER_SRC))}).href);
const results = {json.dumps(passwords)}.map((pwd) => mod.parseRescueMail(pwd));
process.stdout.write(JSON.stringify({{ results }}));
""")
        for (kind_num, mail, _pwd), parsed in zip(cases, out["results"]):
            self.assertIsNotNone(parsed)
            self.assertEqual(parsed["kind"], kind_num)
            self.assertEqual(parsed["dungeonId"], mail.dungeon_id)
            self.assertEqual(parsed["floor"], mail.floor)
            self.assertEqual(parsed["clientSpecies"], mail.client_species)
            self.assertEqual(parsed["rescueId"], mail.rescue_id)
            self.assertEqual(parsed["revivesLeft"], mail.revives_left)
            self.assertEqual(parsed["teamName"], mail.team_name)

    def test_stateful_lifecycle_identity_auth_and_expiry(self) -> None:
        sos = RescueMailData("SOS", 24, 15, "Pokepals", 393, 0x9ABCDEF0, 3)
        sos_pwd = encode_rescue_mail(sos)
        aok = create_aok_from_sos(sos, rescuer_team="Saviors")
        aok_pwd = encode_rescue_mail(aok)
        unrelated_aok_pwd = encode_rescue_mail(
            RescueMailData("A-OK", 24, 15, "Saviors", 393, 0x11112222, 2)
        )
        wrong_species_aok_pwd = encode_rescue_mail(
            RescueMailData("A-OK", 24, 15, "Saviors", 25, 0x9ABCDEF0, 2)
        )
        ty_pwd = encode_rescue_mail(create_thank_you_from_aok(aok, team_name="Pokepals"))
        wrong_ty_pwd = encode_rescue_mail(
            RescueMailData("THANK-YOU", 24, 15, "Pokepals", 393, 0x22223333, 2)
        )
        wrong_species_ty_pwd = encode_rescue_mail(
            RescueMailData("THANK-YOU", 24, 15, "Pokepals", 25, 0x9ABCDEF0, 2)
        )

        out = _run_node_script(f"""
import {{ pathToFileURL }} from "node:url";
const mod = await import(pathToFileURL({json.dumps(str(WORKER_SRC))}).href);
const worker = mod.default;
{MOCK_D1_JS}
const db = new MockD1Database();
const env = {{ DB: db, MAX_CLAIM_DURATION_HOURS: "24" }};
async function call(method, path, body) {{
  const init = {{ method, headers: {{}} }};
  if (body !== undefined) {{
    init.headers["Content-Type"] = "application/json";
    init.body = typeof body === "string" ? body : JSON.stringify(body);
  }}
  const r = await worker.fetch(new Request("https://rescue.example" + path, init), env);
  return {{ status: r.status, json: await r.json() }};
}}

const out = {{}};
out.health = await call("GET", "/health");
out.create = await call("POST", "/v1/rescues", {{
  dungeon_id: 24, floor: 15, team_name: "Pokepals", client_species: 393,
  sos_password: {json.dumps(sos_pwd)}, friend_code: "1234-5678-9012"
}});
out.dupCreate = await call("POST", "/v1/rescues", {{
  dungeon_id: 24, floor: 15, team_name: "Pokepals", client_species: 393,
  sos_password: {json.dumps(sos_pwd)}
}});
const id = out.create.json.id;
const ownerToken = out.create.json.owner_token;

out.listOpen = await call("GET", "/v1/rescues?status=open");
out.getSingle = await call("GET", "/v1/rescues/" + id);
out.claim1 = await call("POST", "/v1/rescues/" + id + "/claim", {{ rescuer_team_name: "Saviors" }});
const claimToken1 = out.claim1.json.claim_token;
out.claim2 = await call("POST", "/v1/rescues/" + id + "/claim", {{ rescuer_team_name: "Rivals" }});

// Expire claim, list open rescues (reverts), then re-claim by Rivals
db.rows[0].claim_expires_at = Math.floor(Date.now() / 1000) - 10;
out.listAfterExpire = await call("GET", "/v1/rescues?status=open");
out.claimReclaimed = await call("POST", "/v1/rescues/" + id + "/claim", {{ rescuer_team_name: "Rivals" }});
const claimToken2 = out.claimReclaimed.json.claim_token;

out.completeWrongToken = await call("POST", "/v1/rescues/" + id + "/complete", {{
  aok_password: {json.dumps(aok_pwd)}, claim_token: claimToken1
}});
out.completeWrongId = await call("POST", "/v1/rescues/" + id + "/complete", {{
  aok_password: {json.dumps(unrelated_aok_pwd)}, claim_token: claimToken2
}});
out.completeWrongSpecies = await call("POST", "/v1/rescues/" + id + "/complete", {{
  aok_password: {json.dumps(wrong_species_aok_pwd)}, claim_token: claimToken2
}});
out.thankYouEarly = await call("POST", "/v1/rescues/" + id + "/thank-you", {{
  thank_you_password: {json.dumps(ty_pwd)}, owner_token: ownerToken
}});
out.completeOk = await call("POST", "/v1/rescues/" + id + "/complete", {{
  aok_password: {json.dumps(aok_pwd)}, claim_token: claimToken2
}});
out.completeDup = await call("POST", "/v1/rescues/" + id + "/complete", {{
  aok_password: {json.dumps(aok_pwd)}, claim_token: claimToken2
}});
out.tyWrongOwner = await call("POST", "/v1/rescues/" + id + "/thank-you", {{
  thank_you_password: {json.dumps(ty_pwd)}, owner_token: "wrong-token"
}});
out.tyWrongId = await call("POST", "/v1/rescues/" + id + "/thank-you", {{
  thank_you_password: {json.dumps(wrong_ty_pwd)}, owner_token: ownerToken
}});
out.tyWrongSpecies = await call("POST", "/v1/rescues/" + id + "/thank-you", {{
  thank_you_password: {json.dumps(wrong_species_ty_pwd)}, owner_token: ownerToken
}});
out.tyOk = await call("POST", "/v1/rescues/" + id + "/thank-you", {{
  thank_you_password: {json.dumps(ty_pwd)}, owner_token: ownerToken
}});
out.tyDup = await call("POST", "/v1/rescues/" + id + "/thank-you", {{
  thank_you_password: {json.dumps(ty_pwd)}, owner_token: ownerToken
}});
out.finalGet = await call("GET", "/v1/rescues/" + id);
out.badStatus = await call("GET", "/v1/rescues?status=bogus");
out.badDungeon = await call("GET", "/v1/rescues?dungeon_id=abc");
out.badJson = await call("POST", "/v1/rescues", "{{not json");
process.stdout.write(JSON.stringify(out));
""")

        self.assertEqual(out["health"]["status"], 200)
        self.assertEqual(out["create"]["status"], 201)
        self.assertEqual(out["create"]["json"]["rescue_id"], 0x9ABCDEF0)
        self.assertEqual(out["dupCreate"]["status"], 409)

        list_row = out["listOpen"]["json"]["rescues"][0]
        self.assertNotIn("friend_code", list_row)
        self.assertNotIn("owner_token", list_row)
        self.assertNotIn("claim_token", list_row)

        single_row = out["getSingle"]["json"]["rescue"]
        self.assertNotIn("friend_code", single_row)
        self.assertNotIn("owner_token", single_row)
        self.assertNotIn("claim_token", single_row)

        self.assertEqual(out["claim1"]["status"], 200)
        self.assertEqual(out["claim2"]["status"], 409)
        self.assertEqual(out["listAfterExpire"]["status"], 200)
        self.assertEqual(out["listAfterExpire"]["json"]["count"], 1)
        self.assertEqual(out["claimReclaimed"]["status"], 200)
        self.assertEqual(out["completeWrongToken"]["status"], 403)
        self.assertEqual(out["completeWrongId"]["status"], 400)
        self.assertEqual(out["completeWrongSpecies"]["status"], 400)
        self.assertEqual(out["thankYouEarly"]["status"], 409)
        self.assertEqual(out["completeOk"]["status"], 200)
        self.assertEqual(out["completeDup"]["status"], 409)
        self.assertEqual(out["tyWrongOwner"]["status"], 403)
        self.assertEqual(out["tyWrongId"]["status"], 400)
        self.assertEqual(out["tyWrongSpecies"]["status"], 400)
        self.assertEqual(out["tyOk"]["status"], 200)
        self.assertTrue(out["tyOk"]["json"]["thank_you_recorded"])
        self.assertEqual(out["tyDup"]["status"], 409)

        final_rescue = out["finalGet"]["json"]["rescue"]
        self.assertEqual(final_rescue["status"], "completed")
        self.assertEqual(final_rescue["aok_password"], aok_pwd)
        self.assertEqual(final_rescue["thank_you_password"], ty_pwd)
        self.assertNotIn("friend_code", final_rescue)
        self.assertNotIn("owner_token", final_rescue)
        self.assertNotIn("claim_token", final_rescue)
        self.assertEqual(out["badStatus"]["status"], 400)
        self.assertEqual(out["badDungeon"]["status"], 400)
        self.assertEqual(out["badJson"]["status"], 400)


if __name__ == "__main__":
    unittest.main()
