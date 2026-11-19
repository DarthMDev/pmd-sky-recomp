-- services/rescue-worker/schema.sql
-- Cloudflare D1 (SQLite) Schema for SkyRecompiled Online Friend Rescue Board

CREATE TABLE IF NOT EXISTS rescues (
    id TEXT PRIMARY KEY,
    rescue_id INTEGER NOT NULL DEFAULT 0,
    dungeon_id INTEGER NOT NULL,
    floor INTEGER NOT NULL,
    team_name TEXT NOT NULL,
    client_species INTEGER NOT NULL,
    sos_password TEXT NOT NULL,
    aok_password TEXT,
    thank_you_password TEXT,
    status TEXT NOT NULL DEFAULT 'open', -- 'open', 'claimed', 'completed', 'expired'
    rescuer_team_name TEXT,
    friend_code TEXT,
    owner_token TEXT,
    claim_token TEXT,
    claim_expires_at INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_rescues_status ON rescues(status);
CREATE INDEX IF NOT EXISTS idx_rescues_dungeon ON rescues(dungeon_id, floor);
CREATE INDEX IF NOT EXISTS idx_rescues_rescue_id ON rescues(rescue_id);
CREATE UNIQUE INDEX IF NOT EXISTS idx_rescues_active_rescue_id
    ON rescues(rescue_id) WHERE status IN ('open', 'claimed');
CREATE UNIQUE INDEX IF NOT EXISTS idx_rescues_active_sos_password
    ON rescues(sos_password) WHERE status IN ('open', 'claimed');
CREATE INDEX IF NOT EXISTS idx_rescues_created_at ON rescues(created_at DESC);
