-- Versioned migration for existing SkyRecompiled D1 databases.
-- Adds rescue identity, Thank-You mail, participant tokens, and active-rescue uniqueness.
-- Note: Use scripts/apply_migrations.mjs (npm run d1:migrate-local / d1:migrate-remote)
-- so legacy rows with rescue_id = -1 are backfilled from sos_password before creating
-- idx_rescues_active_rescue_id.

ALTER TABLE rescues ADD COLUMN rescue_id INTEGER NOT NULL DEFAULT -1;
ALTER TABLE rescues ADD COLUMN thank_you_password TEXT;
ALTER TABLE rescues ADD COLUMN owner_token TEXT;
ALTER TABLE rescues ADD COLUMN claim_token TEXT;
