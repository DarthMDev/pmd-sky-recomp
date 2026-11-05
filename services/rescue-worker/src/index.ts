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

const WONDER_MAIL_CHARSET = new Set("&67NPR89F0+#STXY45MCHJ-K12=%3Q@W");

function isValidPassword(pwd: string, expectedLen: number): boolean {
  const clean = pwd.replace(/[\s\r\n-]/g, "").toUpperCase();
  if (clean.length !== expectedLen) return false;
  for (let i = 0; i < clean.length; i++) {
    if (!WONDER_MAIL_CHARSET.has(clean[i])) return false;
  }
  return true;
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
        const body = await request.json() as {
          dungeon_id?: number;
          floor?: number;
          team_name?: string;
          client_species?: number;
          sos_password?: string;
          friend_code?: string;
        };

        if (
          body.dungeon_id === undefined ||
          body.floor === undefined ||
          !body.team_name ||
          body.client_species === undefined ||
          !body.sos_password
        ) {
          return errorResponse("Missing required fields (dungeon_id, floor, team_name, client_species, sos_password)");
        }

        const cleanSos = body.sos_password.replace(/[\s\r\n-]/g, "").toUpperCase();
        if (!isValidPassword(cleanSos, 54)) {
          return errorResponse("Invalid SOS password: must be 54 characters from Explorers of Sky charset");
        }

        const id = crypto.randomUUID();
        const teamName = body.team_name.slice(0, 10);
        const friendCode = body.friend_code ? body.friend_code.slice(0, 20) : null;

        await env.DB.prepare(
          `INSERT INTO rescues (
            id, dungeon_id, floor, team_name, client_species, sos_password,
            status, friend_code, created_at, updated_at
          ) VALUES (?, ?, ?, ?, ?, ?, 'open', ?, ?, ?)`
        )
          .bind(
            id,
            body.dungeon_id,
            body.floor,
            teamName,
            body.client_species,
            cleanSos,
            friendCode,
            now,
            now
          )
          .run();

        return jsonResponse({
          success: true,
          id,
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
          `UPDATE rescues SET status = 'open', claim_expires_at = NULL, rescuer_team_name = NULL, updated_at = ?
           WHERE status = 'claimed' AND claim_expires_at IS NOT NULL AND claim_expires_at < ?`
        )
          .bind(now, now)
          .run();

        const statusParam = url.searchParams.get("status") || "open";
        const dungeonId = url.searchParams.get("dungeon_id");
        const limit = Math.min(Math.max(parseInt(url.searchParams.get("limit") || "50", 10), 1), 100);
        const offset = Math.max(parseInt(url.searchParams.get("offset") || "0", 10), 0);

        let query = "SELECT * FROM rescues WHERE status = ?";
        const bindings: (string | number)[] = [statusParam];

        if (dungeonId) {
          query += " AND dungeon_id = ?";
          bindings.push(parseInt(dungeonId, 10));
        }

        query += " ORDER BY created_at DESC LIMIT ? OFFSET ?";
        bindings.push(limit, offset);

        const { results } = await env.DB.prepare(query).bind(...bindings).all();

        return jsonResponse({
          success: true,
          count: results.length,
          rescues: results,
        });
      }

      // ───────────────────────────────────────────────────────────────────────
      // Routes on /v1/rescues/:id
      // ───────────────────────────────────────────────────────────────────────
      const match = path.match(/^\/v1\/rescues\/([a-f0-9-]+)(\/([a-z]+))?$/i);
      if (match) {
        const rescueId = match[1];
        const subAction = match[3]; // 'claim' or 'complete' or undefined

        // GET /v1/rescues/:id
        if (!subAction && request.method === "GET") {
          const rescue = await env.DB.prepare("SELECT * FROM rescues WHERE id = ?").bind(rescueId).first();
          if (!rescue) {
            return errorResponse("Rescue not found", 404);
          }
          return jsonResponse({ success: true, rescue });
        }

        // POST /v1/rescues/:id/claim
        if (subAction === "claim" && request.method === "POST") {
          const body = await request.json() as { rescuer_team_name?: string };
          if (!body.rescuer_team_name) {
            return errorResponse("Missing required field: rescuer_team_name");
          }

          const hours = parseInt(env.MAX_CLAIM_DURATION_HOURS || "24", 10);
          const claimExpires = now + hours * 3600;
          const rescuerTeam = body.rescuer_team_name.slice(0, 10);

          const res = await env.DB.prepare(
            `UPDATE rescues 
             SET status = 'claimed', rescuer_team_name = ?, claim_expires_at = ?, updated_at = ?
             WHERE id = ? AND (status = 'open' OR (status = 'claimed' AND claim_expires_at < ?))`
          )
            .bind(rescuerTeam, claimExpires, now, rescueId, now)
            .run();

          if (res.meta.changes === 0) {
            return errorResponse("Rescue is already claimed or no longer available", 409);
          }

          return jsonResponse({
            success: true,
            id: rescueId,
            status: "claimed",
            claim_expires_at: claimExpires,
            message: `Rescue claimed by Team ${rescuerTeam}! You have ${hours} hours to complete the rescue.`,
          });
        }

        // POST /v1/rescues/:id/complete
        if (subAction === "complete" && request.method === "POST") {
          const body = await request.json() as { aok_password?: string };
          if (!body.aok_password) {
            return errorResponse("Missing required field: aok_password");
          }

          const cleanAok = body.aok_password.replace(/[\s\r\n-]/g, "").toUpperCase();
          if (!isValidPassword(cleanAok, 54)) {
            return errorResponse("Invalid A-OK password: must be 54 characters from Explorers of Sky charset");
          }

          const res = await env.DB.prepare(
            `UPDATE rescues 
             SET status = 'completed', aok_password = ?, updated_at = ?
             WHERE id = ? AND status IN ('open', 'claimed')`
          )
            .bind(cleanAok, now, rescueId)
            .run();

          if (res.meta.changes === 0) {
            return errorResponse("Rescue not found or already completed", 404);
          }

          return jsonResponse({
            success: true,
            id: rescueId,
            status: "completed",
            message: "A-OK Mail accepted! The fallen team can now be revived.",
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
