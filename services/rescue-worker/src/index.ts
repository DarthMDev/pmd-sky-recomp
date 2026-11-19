/**
 * services/rescue-worker/src/index.ts
 * Cloudflare Worker + D1 Backend for SkyRecompiled Online Friend Rescue Board.
 * 
 * Provides an asynchronous online rescue board for Pokémon Mystery Dungeon: Explorers of Sky.
 * 100% compatible with official 54-character SOS/A-OK passwords.
 */

export interface Env {
  DB: D1Database;
  ENVIRONMENT?: string;
  MAX_CLAIM_DURATION_HOURS?: string;
}

export interface ParsedRescueMail {
  kind: number;
  dungeonId: number;
  floor: number;
  clientSpecies: number;
  rescueId: number;
  revivesLeft: number;
  teamName: string;
}

const WONDER_MAIL_CHARSET_STR = "&67NPR89F0+#STXY45MCHJ-K12=%3Q@W";
const WONDER_MAIL_CHARSET = new Set(WONDER_MAIL_CHARSET_STR);
const CHAR_MAP = new Map<string, number>();
for (let i = 0; i < WONDER_MAIL_CHARSET_STR.length; i++) {
  CHAR_MAP.set(WONDER_MAIL_CHARSET_STR[i], i);
}

const VALID_STATUSES = new Set(["open", "claimed", "completed", "expired"]);

export function normalizePassword(pwd: string): string {
  // Only strip whitespace and newlines; preserve '-' as it is a valid charset symbol!
  return pwd.replace(/\s+/g, "").toUpperCase();
}

function isValidPassword(clean: string, expectedLen: number): boolean {
  if (clean.length !== expectedLen) return false;
  for (let i = 0; i < clean.length; i++) {
    if (!WONDER_MAIL_CHARSET.has(clean[i])) return false;
  }
  return true;
}

function computeChecksum(symbols: number[]): number {
  let c1 = 0;
  let c2 = 0;
  for (let i = 0; i < 52; i++) {
    c1 = (c1 + symbols[i]) & 0x1F;
    c2 = (c2 + c1) & 0x1F;
  }
  return (c1 << 5) | c2;
}

export function parseRescueMail(clean: string): ParsedRescueMail | null {
  if (clean.length !== 54) return null;
  const symbols: number[] = [];
  for (let i = 0; i < clean.length; i++) {
    const val = CHAR_MAP.get(clean[i]);
    if (val === undefined) return null;
    symbols.push(val);
  }

  const expectedChk = (symbols[52] << 5) | symbols[53];
  const actualChk = computeChecksum(symbols);
  if (expectedChk !== actualChk) return null;

  const bits: number[] = [];
  for (let i = 0; i < 52; i++) {
    for (let b = 4; b >= 0; b--) {
      bits.push((symbols[i] >> b) & 1);
    }
  }

  let bitPos = 0;
  function readBits(n: number): number {
    let val = 0;
    for (let i = 0; i < n; i++) {
      val = val * 2 + bits[bitPos++];
    }
    return val;
  }

  const kind = readBits(3); // 1 = SOS, 2 = A-OK, 3 = THANK-YOU
  if (kind < 1 || kind > 3) return null;
  const dungeonId = readBits(7);
  const floor = readBits(7);
  const clientSpecies = readBits(11);
  const rescueId = readBits(32);
  const revivesLeft = readBits(3);
  const nameLen = readBits(4);
  if (nameLen > 10) return null;

  let teamName = "";
  for (let i = 0; i < nameLen; i++) {
    teamName += String.fromCharCode(readBits(8));
  }

  return { kind, dungeonId, floor, clientSpecies, rescueId, revivesLeft, teamName };
}

function sanitizeRescueRow(row: Record<string, unknown>): Record<string, unknown> {
  const sanitized: Record<string, unknown> = { ...row };
  delete sanitized.friend_code;
  delete sanitized.owner_token;
  delete sanitized.claim_token;
  return sanitized;
}

async function readJsonBody<T>(request: Request): Promise<T | null> {
  try {
    const parsed = await request.json();
    if (!parsed || typeof parsed !== "object" || Array.isArray(parsed)) {
      return null;
    }
    return parsed as T;
  } catch {
    return null;
  }
}

const CORS_HEADERS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, Authorization",
  "Content-Type": "application/json",
};

function jsonResponse(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: CORS_HEADERS,
  });
}

function errorResponse(message: string, status = 400): Response {
  return jsonResponse({ error: message, success: false }, status);
}

function resolveExpectedRescueId(row: { rescue_id?: number | null; sos_password?: string | null }): number | null {
  if (typeof row.rescue_id === "number" && row.rescue_id !== 0) {
    return row.rescue_id;
  }
  if (typeof row.sos_password === "string") {
    const parsed = parseRescueMail(row.sos_password);
    if (parsed) return parsed.rescueId;
  }
  return typeof row.rescue_id === "number" ? row.rescue_id : null;
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    if (request.method === "OPTIONS") {
      return new Response(null, { headers: CORS_HEADERS });
    }

    const url = new URL(request.url);
    const path = url.pathname;
    const now = Math.floor(Date.now() / 1000);

    try {
      // ───────────────────────────────────────────────────────────────────────
      // Health check
      // ───────────────────────────────────────────────────────────────────────
      if (path === "/health" || path === "/") {
        return jsonResponse({
          status: "healthy",
          service: "SkyRecompiled Online Rescue Board",
          timestamp: now,
        });
      }

      // ───────────────────────────────────────────────────────────────────────
      // POST /v1/rescues (Submit SOS Request)
      // ───────────────────────────────────────────────────────────────────────
      if (path === "/v1/rescues" && request.method === "POST") {
        const body = await readJsonBody<{
          dungeon_id?: number;
          floor?: number;
          team_name?: string;
          client_species?: number;
          sos_password?: string;
          friend_code?: string;
        }>(request);
        if (!body) {
          return errorResponse("Invalid JSON request body", 400);
        }

        if (
          typeof body.dungeon_id !== "number" ||
          !Number.isInteger(body.dungeon_id) ||
          typeof body.floor !== "number" ||
          !Number.isInteger(body.floor) ||
          typeof body.team_name !== "string" ||
          body.team_name.length === 0 ||
          typeof body.client_species !== "number" ||
          !Number.isInteger(body.client_species) ||
          typeof body.sos_password !== "string" ||
          body.sos_password.length === 0
        ) {
          return errorResponse("Missing or invalid required fields (dungeon_id, floor, team_name, client_species, sos_password)");
        }

        const cleanSos = normalizePassword(body.sos_password);
        if (!isValidPassword(cleanSos, 54)) {
          return errorResponse("Invalid SOS password: must be 54 characters from Explorers of Sky charset");
        }

        const sosInfo = parseRescueMail(cleanSos);
        if (!sosInfo || sosInfo.kind !== 1) {
          return errorResponse("Invalid SOS password: checksum verification failed or not an SOS request");
        }
        const teamName = body.team_name.slice(0, 10);
        if (
          sosInfo.dungeonId !== body.dungeon_id ||
          sosInfo.floor !== body.floor ||
          sosInfo.clientSpecies !== body.client_species ||
          sosInfo.teamName !== teamName
        ) {
          return errorResponse("SOS password metadata does not match submission fields");
        }

        const existing = await env.DB.prepare(
          "SELECT id FROM rescues WHERE status IN ('open', 'claimed') AND (rescue_id = ? OR sos_password = ?)"
        )
          .bind(sosInfo.rescueId, cleanSos)
          .first();
        if (existing) {
          return errorResponse("An active rescue with this SOS identity already exists", 409);
        }

        const id = crypto.randomUUID();
        const ownerToken = crypto.randomUUID();
        const friendCode = typeof body.friend_code === "string" ? body.friend_code.slice(0, 20) : null;

        await env.DB.prepare(
          `INSERT INTO rescues (
            id, rescue_id, dungeon_id, floor, team_name, client_species, sos_password,
            status, friend_code, owner_token, created_at, updated_at
          ) VALUES (?, ?, ?, ?, ?, ?, ?, 'open', ?, ?, ?, ?)`
        )
          .bind(
            id,
            sosInfo.rescueId,
            body.dungeon_id,
            body.floor,
            teamName,
            body.client_species,
            cleanSos,
            friendCode,
            ownerToken,
            now,
            now
          )
          .run();

        return jsonResponse({
          success: true,
          id,
          rescue_id: sosInfo.rescueId,
          owner_token: ownerToken,
          status: "open",
          message: "Rescue request posted successfully to the Pelipper Board!",
        }, 201);
      }

      // ───────────────────────────────────────────────────────────────────────
      // GET /v1/rescues (List Rescues)
      // ───────────────────────────────────────────────────────────────────────
      if (path === "/v1/rescues" && request.method === "GET") {
        // Auto-revert expired claims back to 'open'
        await env.DB.prepare(
          `UPDATE rescues SET status = 'open', claim_expires_at = NULL, rescuer_team_name = NULL, claim_token = NULL, updated_at = ?
           WHERE status = 'claimed' AND claim_expires_at IS NOT NULL AND claim_expires_at < ?`
        )
          .bind(now, now)
          .run();

        const statusParam = url.searchParams.get("status") || "open";
        if (!VALID_STATUSES.has(statusParam)) {
          return errorResponse("Invalid status filter", 400);
        }

        const dungeonIdRaw = url.searchParams.get("dungeon_id");
        let parsedDungeonId: number | null = null;
        if (dungeonIdRaw !== null) {
          if (!/^\d+$/.test(dungeonIdRaw)) {
            return errorResponse("Invalid dungeon_id parameter", 400);
          }
          parsedDungeonId = parseInt(dungeonIdRaw, 10);
          if (parsedDungeonId < 0 || parsedDungeonId > 127) {
            return errorResponse("Invalid dungeon_id parameter", 400);
          }
        }

        const limitRaw = url.searchParams.get("limit");
        const offsetRaw = url.searchParams.get("offset");
        if (limitRaw !== null && !/^\d+$/.test(limitRaw)) {
          return errorResponse("Invalid limit parameter", 400);
        }
        if (offsetRaw !== null && !/^\d+$/.test(offsetRaw)) {
          return errorResponse("Invalid offset parameter", 400);
        }
        const limit = Math.min(Math.max(parseInt(limitRaw || "50", 10), 1), 100);
        const offset = Math.max(parseInt(offsetRaw || "0", 10), 0);

        let query = "SELECT * FROM rescues WHERE status = ?";
        const bindings: (string | number)[] = [statusParam];

        if (parsedDungeonId !== null) {
          query += " AND dungeon_id = ?";
          bindings.push(parsedDungeonId);
        }

        query += " ORDER BY created_at DESC LIMIT ? OFFSET ?";
        bindings.push(limit, offset);

        const { results } = await env.DB.prepare(query).bind(...bindings).all();
        const sanitized = results.map((row) => sanitizeRescueRow(row as Record<string, unknown>));

        return jsonResponse({
          success: true,
          count: sanitized.length,
          rescues: sanitized,
        });
      }

      // ───────────────────────────────────────────────────────────────────────
      // Routes on /v1/rescues/:id
      // ───────────────────────────────────────────────────────────────────────
      const match = path.match(/^\/v1\/rescues\/([a-f0-9-]+)(\/([a-z-]+))?$/i);
      if (match) {
        const rescueId = match[1];
        const subAction = match[3]; // 'claim', 'complete', 'thank-you', or undefined

        // GET /v1/rescues/:id
        if (!subAction && request.method === "GET") {
          const rescue = await env.DB.prepare("SELECT * FROM rescues WHERE id = ?").bind(rescueId).first();
          if (!rescue) {
            return errorResponse("Rescue not found", 404);
          }
          return jsonResponse({
            success: true,
            rescue: sanitizeRescueRow(rescue as Record<string, unknown>),
          });
        }

        // POST /v1/rescues/:id/claim
        if (subAction === "claim" && request.method === "POST") {
          const body = await readJsonBody<{ rescuer_team_name?: string }>(request);
          if (!body || typeof body.rescuer_team_name !== "string" || body.rescuer_team_name.trim().length === 0) {
            return errorResponse("Missing required field: rescuer_team_name");
          }

          const hours = Math.max(1, parseInt(env.MAX_CLAIM_DURATION_HOURS || "24", 10) || 24);
          const claimExpires = now + hours * 3600;
          const rescuerTeam = body.rescuer_team_name.slice(0, 10);
          const claimToken = crypto.randomUUID();

          const res = await env.DB.prepare(
            `UPDATE rescues
             SET status = 'claimed', rescuer_team_name = ?, claim_token = ?, claim_expires_at = ?, updated_at = ?
             WHERE id = ? AND (status = 'open' OR (status = 'claimed' AND claim_expires_at < ?))`
          )
            .bind(rescuerTeam, claimToken, claimExpires, now, rescueId, now)
            .run();

          if (res.meta.changes === 0) {
            return errorResponse("Rescue is already claimed or no longer available", 409);
          }

          return jsonResponse({
            success: true,
            id: rescueId,
            status: "claimed",
            claim_token: claimToken,
            claim_expires_at: claimExpires,
            message: `Rescue claimed by Team ${rescuerTeam}! You have ${hours} hours to complete the rescue.`,
          });
        }

        // POST /v1/rescues/:id/complete
        if (subAction === "complete" && request.method === "POST") {
          const body = await readJsonBody<{ aok_password?: string; claim_token?: string }>(request);
          if (!body || typeof body.aok_password !== "string" || body.aok_password.length === 0) {
            return errorResponse("Missing required field: aok_password");
          }

          const cleanAok = normalizePassword(body.aok_password);
          if (!isValidPassword(cleanAok, 54)) {
            return errorResponse("Invalid A-OK password: must be 54 characters from Explorers of Sky charset");
          }

          const aokInfo = parseRescueMail(cleanAok);
          if (!aokInfo || aokInfo.kind !== 2) {
            return errorResponse("Invalid A-OK password: checksum verification failed or not an A-OK completion");
          }

          const rescue = await env.DB.prepare(
            "SELECT id, rescue_id, dungeon_id, floor, sos_password, status, claim_token, claim_expires_at FROM rescues WHERE id = ?"
          )
            .bind(rescueId)
            .first<{
              id: string;
              rescue_id: number | null;
              dungeon_id: number;
              floor: number;
              sos_password: string;
              status: string;
              claim_token: string | null;
              claim_expires_at: number | null;
            }>();

          if (!rescue) {
            return errorResponse("Rescue not found", 404);
          }
          if (rescue.status === "completed") {
            return errorResponse("Rescue is already completed", 409);
          }
          if (rescue.status === "expired") {
            return errorResponse("Rescue has expired", 409);
          }

          const expectedRescueId = resolveExpectedRescueId(rescue);
          if (
            aokInfo.dungeonId !== rescue.dungeon_id ||
            aokInfo.floor !== rescue.floor ||
            expectedRescueId === null ||
            aokInfo.rescueId !== expectedRescueId
          ) {
            return errorResponse("A-OK password does not match this rescue mission's identity, dungeon, or floor");
          }

          if (
            rescue.status === "claimed" &&
            rescue.claim_token &&
            (rescue.claim_expires_at ?? 0) >= now &&
            body.claim_token !== rescue.claim_token
          ) {
            return errorResponse("Valid claim_token is required to complete a claimed rescue", 403);
          }

          const res = await env.DB.prepare(
            `UPDATE rescues
             SET status = 'completed', aok_password = ?, updated_at = ?
             WHERE id = ? AND status IN ('open', 'claimed')`
          )
            .bind(cleanAok, now, rescueId)
            .run();

          if (res.meta.changes === 0) {
            return errorResponse("Rescue is already completed or unavailable", 409);
          }

          return jsonResponse({
            success: true,
            id: rescueId,
            rescue_id: expectedRescueId,
            status: "completed",
            message: "A-OK Mail accepted! The fallen team can now be revived.",
          });
        }

        // POST /v1/rescues/:id/thank-you
        if (subAction === "thank-you" && request.method === "POST") {
          const body = await readJsonBody<{ thank_you_password?: string; owner_token?: string }>(request);
          if (!body || typeof body.thank_you_password !== "string" || body.thank_you_password.length === 0) {
            return errorResponse("Missing required field: thank_you_password");
          }

          const cleanTy = normalizePassword(body.thank_you_password);
          if (!isValidPassword(cleanTy, 54)) {
            return errorResponse("Invalid Thank-You password: must be 54 characters from Explorers of Sky charset");
          }

          const tyInfo = parseRescueMail(cleanTy);
          if (!tyInfo || tyInfo.kind !== 3) {
            return errorResponse("Invalid Thank-You password: checksum verification failed or not a Thank-You mail");
          }

          const rescue = await env.DB.prepare(
            "SELECT id, rescue_id, dungeon_id, floor, sos_password, status, owner_token, thank_you_password FROM rescues WHERE id = ?"
          )
            .bind(rescueId)
            .first<{
              id: string;
              rescue_id: number | null;
              dungeon_id: number;
              floor: number;
              sos_password: string;
              status: string;
              owner_token: string | null;
              thank_you_password: string | null;
            }>();

          if (!rescue) {
            return errorResponse("Rescue not found", 404);
          }
          if (rescue.status !== "completed") {
            return errorResponse("Rescue must be completed before submitting Thank-You mail", 409);
          }
          if (rescue.thank_you_password) {
            return errorResponse("Thank-You mail has already been submitted for this rescue", 409);
          }
          if (rescue.owner_token && body.owner_token !== rescue.owner_token) {
            return errorResponse("Valid owner_token is required to submit Thank-You mail", 403);
          }

          const expectedRescueId = resolveExpectedRescueId(rescue);
          if (
            tyInfo.dungeonId !== rescue.dungeon_id ||
            tyInfo.floor !== rescue.floor ||
            expectedRescueId === null ||
            tyInfo.rescueId !== expectedRescueId
          ) {
            return errorResponse("Thank-You password does not match this rescue mission's identity, dungeon, or floor");
          }

          const res = await env.DB.prepare(
            `UPDATE rescues
             SET thank_you_password = ?, updated_at = ?
             WHERE id = ? AND status = 'completed' AND thank_you_password IS NULL`
          )
            .bind(cleanTy, now, rescueId)
            .run();

          if (res.meta.changes === 0) {
            return errorResponse("Thank-You mail has already been recorded or rescue is not completed", 409);
          }

          return jsonResponse({
            success: true,
            id: rescueId,
            status: "completed",
            thank_you_recorded: true,
            message: "Thank-You Mail delivered to your rescuer!",
          });
        }
      }

      return errorResponse("Endpoint not found", 404);
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : String(err);
      return errorResponse(`Internal server error: ${msg}`, 500);
    }
  },
};
