# SkyRecompiled Online Friend Rescue Service

An asynchronous online rescue board for *Pokémon Mystery Dungeon: Explorers of Sky*, designed to run at near-\$0 cost on **Cloudflare Workers** and **Cloudflare D1 (Serverless SQLite)**.

## Features
- **100% Offline Compatible:** Players can copy and paste official 54-character SOS/A-OK passwords or use this REST API.
- **\$0/mo Free Tier Deployment:** Operates within Cloudflare's generous free tier:
  - 100,000 Worker requests/day
  - 5,000,000 D1 row reads/day
  - 100,000 D1 row writes/day
- **Rescue Claim Leases:** 24-hour claim leases automatically revert back to `open` if a rescuer abandons a mission.
- **CORS Enabled:** Seamlessly queried from the native `SkyRecompiled` launcher, a web portal, or Discord bots.

## Live Production Endpoint

- **Production API URL:** `https://sky-rescue-worker.darthanonymous1.workers.dev/`
- **Health Check:** `https://sky-rescue-worker.darthanonymous1.workers.dev/health`

## Endpoints

| Method | Endpoint | Description |
| :--- | :--- | :--- |
| `GET` | `/health` | Health check endpoint |
| `POST` | `/v1/rescues` | Submit a new SOS rescue request (54-char password) |
| `GET` | `/v1/rescues` | List open rescues (supports `?dungeon_id=...&status=open&limit=50`) |
| `GET` | `/v1/rescues/:id` | Get details and status of a single rescue |
| `POST` | `/v1/rescues/:id/claim` | Claim an open rescue with your team name (24-hour lease) |
| `POST` | `/v1/rescues/:id/complete` | Complete the rescue with a 54-char A-OK password |

## Local Development

```bash
cd services/rescue-worker

# 1. Install dependencies
npm install

# 2. Initialize local SQLite D1 database
npm run d1:init-local

# 3. Start local development server (runs at http://localhost:8787)
npm run dev
```

## Cloudflare Deployment

```bash
# 1. Create a remote D1 database
npx wrangler d1 create sky_rescues_d1

# 2. Update wrangler.toml with the database_id printed by Cloudflare

# 3. Run migrations on the remote database
npm run d1:init-remote

# 4. Deploy worker to Cloudflare's global edge network
npm run deploy
```
