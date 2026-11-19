#!/usr/bin/env node
/**
 * Idempotent D1 schema + column migration runner for SkyRecompiled rescue-worker.
 * Safe when the rescues table does not exist yet or when columns already exist.
 */
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const workerRoot = path.resolve(__dirname, "..");
const mode = process.argv.includes("--remote") ? "--remote" : "--local";

function runWranglerSql(args) {
  const out = execFileSync(
    "npx",
    ["wrangler", "d1", "execute", "DB", mode, "--json", ...args],
    { cwd: workerRoot, encoding: "utf8" }
  );
  return JSON.parse(out);
}

// Ensure base table & indexes exist for fresh databases.
runWranglerSql(["--file=./schema.sql"]);

// Inspect existing columns and add any missing migration columns safely.
const colRes = runWranglerSql([
  "--command",
  "SELECT name FROM pragma_table_info('rescues');",
]);
const existingCols = new Set(
  (colRes?.[0]?.results ?? []).map((row) => row.name)
);

const missingAlterStatements = [
  ["rescue_id", "ALTER TABLE rescues ADD COLUMN rescue_id INTEGER NOT NULL DEFAULT 0;"],
  ["thank_you_password", "ALTER TABLE rescues ADD COLUMN thank_you_password TEXT;"],
  ["owner_token", "ALTER TABLE rescues ADD COLUMN owner_token TEXT;"],
  ["claim_token", "ALTER TABLE rescues ADD COLUMN claim_token TEXT;"],
];

for (const [col, sql] of missingAlterStatements) {
  if (!existingCols.has(col)) {
    runWranglerSql(["--command", sql]);
  }
}

// Ensure indexes exist after columns are present.
runWranglerSql([
  "--command",
  [
    "CREATE INDEX IF NOT EXISTS idx_rescues_rescue_id ON rescues(rescue_id);",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_rescues_active_rescue_id ON rescues(rescue_id) WHERE status IN ('open', 'claimed');",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_rescues_active_sos_password ON rescues(sos_password) WHERE status IN ('open', 'claimed');",
  ].join(" "),
]);

console.log(`[d1:migrate] Database (${mode}) is up to date.`);
