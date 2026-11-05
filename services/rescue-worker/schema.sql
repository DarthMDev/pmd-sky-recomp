-- services/rescue-worker/schema.sql
-- Cloudflare D1 (SQLite) Schema for SkyRecompiled Online Friend Rescue Board

CREATE TABLE IF NOT EXISTS rescues (
    id TEXT PRIMARY KEY,
    dungeon_id INTEGER NOT NULL,
    floor INTEGER NOT NULL,
    team_name TEXT NOT NULL,
    client_species INTEGER NOT NULL,
    sos_password TEXT NOT NULL,
    aok_password TEXT,
    status TEXT NOT NULL DEFAULT 'open', -- 'open', 'claimed', 'completed', 'expired'
    rescuer_team_name TEXT,
    friend_code TEXT,
    claim_expires_at INTEGER,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_rescues_status ON rescues(status);
CREATE INDEX IF NOT EXISTS idx_rescues_dungeon ON rescues(dungeon_id, floor);
CREATE INDEX IF NOT EXISTS idx_rescues_created_at ON rescues(created_at DESC);
