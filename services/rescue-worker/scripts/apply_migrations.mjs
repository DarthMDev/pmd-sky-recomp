#!/usr/bin/env node
/**
 * Idempotent D1 schema + column migration runner for SkyRecompiled rescue-worker.
 * Ordering for fresh and existing databases:
 *   1. Ensure base rescues table exists (without indexes referencing new columns).
 *   2. Add any missing columns (rescue_id defaults to -1 to distinguish legacy rows from ID 0).
 *   3. Backfill rescue_id for legacy rows by decoding sos_password.
 *   4. Create all indexes, including idx_rescues_active_rescue_id.
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const workerRoot = path.resolve(__dirname, "..");
const mode = process.argv.includes("--remote") ? "--remote" : "--local";

const WONDER_MAIL_CHARSET = "&67NPR89F0+#STXY45MCHJ-K12=%3Q@W";
const CHAR_MAP = new Map([...WONDER_MAIL_CHARSET].map((ch, idx) => [ch, idx]));

export function decodeSosRescueId(rawPassword) {
  const clean = String(rawPassword || "").replace(/\s+/g, "").toUpperCase();
  if (clean.length !== 54) return null;
  const symbols = [];
  for (let i = 0; i < clean.length; i++) {
    const val = CHAR_MAP.get(clean[i]);
    if (val === undefined) return null;
    symbols.push(val);
  }
  let c1 = 0;
  let c2 = 0;
  for (let i = 0; i < 52; i++) {
    c1 = (c1 + symbols[i]) & 0x1F;
    c2 = (c2 + c1) & 0x1F;
  }
  if (((c1 << 5) | c2) !== ((symbols[52] << 5) | symbols[53])) return null;

  const bits = [];
  for (let i = 0; i < 52; i++) {
    for (let b = 4; b >= 0; b--) {
      bits.push((symbols[i] >> b) & 1);
    }
  }
  // Skip kind(3) + dungeonId(7) + floor(7) + clientSpecies(11) = 28 bits, then read rescueId(32).
  let rescueId = 0;
  for (let i = 28; i < 60; i++) {
    rescueId = rescueId * 2 + bits[i];
  }
  return rescueId;
}

function runWranglerSql(args) {
  const out = execFileSync(
    "npx",
    ["wrangler", "d1", "execute", "DB", mode, "--json", ...args],
    { cwd: workerRoot, encoding: "utf8" }
  );
  return JSON.parse(out);
}

// 1. Ensure base rescues table exists before any index or column migration.
const schemaSql = fs.readFileSync(path.join(workerRoot, "schema.sql"), "utf8");
const createTableMatch = schemaSql.match(/CREATE TABLE IF NOT EXISTS rescues\s*\([\s\S]*?\);/i);
if (!createTableMatch) {
  throw new Error("Could not locate CREATE TABLE rescues statement in schema.sql");
}
runWranglerSql(["--command", createTableMatch[0]]);

// 2. Inspect existing columns and add any missing migration columns safely.
const colRes = runWranglerSql([
  "--command",
  "SELECT name FROM pragma_table_info('rescues');",
]);
const existingCols = new Set(
  (colRes?.[0]?.results ?? []).map((row) => row.name)
);

const missingAlterStatements = [
  ["rescue_id", "ALTER TABLE rescues ADD COLUMN rescue_id INTEGER NOT NULL DEFAULT -1;"],
  ["thank_you_password", "ALTER TABLE rescues ADD COLUMN thank_you_password TEXT;"],
  ["owner_token", "ALTER TABLE rescues ADD COLUMN owner_token TEXT;"],
  ["claim_token", "ALTER TABLE rescues ADD COLUMN claim_token TEXT;"],
];

for (const [col, sql] of missingAlterStatements) {
  if (!existingCols.has(col)) {
    runWranglerSql(["--command", sql]);
  }
}

// 3. Backfill legacy rows (marked with rescue_id < 0) before creating unique indexes.
const legacyRes = runWranglerSql([
  "--command",
  "SELECT id, sos_password FROM rescues WHERE rescue_id < 0;",
]);
const legacyRows = legacyRes?.[0]?.results ?? [];
for (const row of legacyRows) {
  const decodedId = decodeSosRescueId(row.sos_password);
  if (decodedId === null) {
    throw new Error(`Cannot backfill rescue_id for row ${row.id}: invalid SOS password`);
  }
  const safeRowId = String(row.id).replace(/'/g, "''");
  runWranglerSql([
    "--command",
    `UPDATE rescues SET rescue_id = ${decodedId} WHERE id = '${safeRowId}';`,
  ]);
}

// 4. Create all indexes only after columns and backfills are complete.
const indexStatements = schemaSql
  .split(";")
  .map((s) => s.trim())
  .filter((s) => /^CREATE\s+(UNIQUE\s+)?INDEX\b/i.test(s))
  .map((s) => `${s};`);

if (indexStatements.length > 0) {
  runWranglerSql(["--command", indexStatements.join(" ")]);
}

console.log(`[d1:migrate] Database (${mode}) is up to date (backfilled ${legacyRows.length} legacy row(s)).`);
