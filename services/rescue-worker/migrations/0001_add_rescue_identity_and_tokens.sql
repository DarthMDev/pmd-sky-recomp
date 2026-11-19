-- Versioned migration for existing SkyRecompiled D1 databases.
-- Adds rescue identity, Thank-You mail, participant tokens, and active-rescue uniqueness.

ALTER TABLE rescues ADD COLUMN rescue_id INTEGER NOT NULL DEFAULT 0;
ALTER TABLE rescues ADD COLUMN thank_you_password TEXT;
ALTER TABLE rescues ADD COLUMN owner_token TEXT;
ALTER TABLE rescues ADD COLUMN claim_token TEXT;

CREATE INDEX IF NOT EXISTS idx_rescues_rescue_id ON rescues(rescue_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_rescues_active_rescue_id
    ON rescues(rescue_id) WHERE status IN ('open', 'claimed');
CREATE UNIQUE INDEX IF NOT EXISTS idx_rescues_active_sos_password
    ON rescues(sos_password) WHERE status IN ('open', 'claimed');
