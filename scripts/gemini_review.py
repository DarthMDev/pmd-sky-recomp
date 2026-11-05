#!/usr/bin/env python3
"""
scripts/gemini_review.py
Automated AI code review using Google Gemini via free-tier Google AI Studio API.
Zero external dependencies (uses standard library urllib and json).
"""

import os
import sys
import json
import subprocess
import urllib.request
import urllib.error

REVIEW_PROMPT = """You are a Principal Systems & Security Architect performing an automated code review on SkyRecompiled (an AOT static recompilation PC port of Pokémon Mystery Dungeon: Explorers of Sky).

Your objective is to perform a rigorous 4-pillar review: Code Quality, Security Audit, Anti-Slop Detection, and Clean-Room Compliance.

Project Invariants & Technical Rules:
1. Clean-Room Discipline: STRICT ZERO-TOLERANCE for proprietary Nintendo/Chunsoft ROM binaries (.nds), extracted binary assets (sprites/soundfonts/scripts), or raw decompiled leaks.
2. Save Safety: Strict EEPROM format (131072 bytes / 128KB), 4-page live validation cap, non-volatile state integrity.
3. Modern Architecture: Strict C++20, zero undefined behavior, multi-platform (macOS/Linux/Windows), BIOS-free direct boot.
4. Hot-Path Performance: Zero dynamic heap allocations in per-frame tick loops; deterministic integer math.

Review the following git diff and output structured Markdown under these exact sections:

### 1. 📋 Summary & Scope
A concise 2-sentence summary of what this diff introduces.

### 2. 🛡️ Security & Memory Safety Audit
- Inspect for buffer overflows, integer overflows, out-of-bounds array accesses, or unsafe pointer arithmetic.
- Inspect tools (mod packers, bitstream decoders, network workers) for path traversal, untrusted input injection, or denial-of-service edge cases.

### 3. 🧹 Anti-Slop & Dead Code Detection
- Flag any AI-generated "slop": hallucinated APIs, redundant wrapper boilerplate, useless type conversions, copy-pasted dead branches, or lazy placeholder comments ("// TODO: implement later").
- Call out over-engineered abstractions that can be simplified.

### 4. ⚖️ Clean-Room & Invariant Compliance
- Verify that no ROM binaries, copyrighted assets, or unlicensed code snippets were introduced.
- Verify adherence to C++20 conventions and project invariants.

### 5. 🎯 Actionable Verdict
- Give a final verdict: **APPROVED (LGTM)**, **APPROVED WITH SUGGESTIONS**, or **REQUEST CHANGES**.
- If changes are needed, provide concrete, minimal diffs or bullet points.

Git Diff to Review:
```diff
{diff_text}
```
"""

def get_git_diff() -> str:
    """Gets the diff against HEAD~1 or origin/main."""
    base_ref = os.environ.get("GITHUB_BASE_REF")
    if base_ref:
        cmd = ["git", "diff", f"origin/{base_ref}...HEAD"]
    else:
        cmd = ["git", "diff", "HEAD~1", "HEAD"]
    
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return res.stdout
    except Exception as e:
        # Fallback to working directory diff if uncommitted
        res = subprocess.run(["git", "diff", "HEAD"], capture_output=True, text=True)
        return res.stdout

def discover_model(api_key: str) -> str:
    """Discovers an available Gemini model that supports generateContent."""
    url = f"https://generativelanguage.googleapis.com/v1beta/models?key={api_key}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "SkyRecompiled-CI/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            models = data.get("models", [])
            valid_models = [
                m["name"] for m in models 
                if "generateContent" in m.get("supportedGenerationMethods", [])
            ]
            
            # Preference ranking (prefer Gemini 3.8 Flash for 2026 API)
            for pref in [
                "models/gemini-3.8-flash",
                "models/gemini-3.8-pro",
                "models/gemini-2.5-flash",
                "models/gemini-2.0-flash",
                "models/gemini-1.5-flash-latest",
                "models/gemini-1.5-flash",
            ]:
                if pref in valid_models:
                    return pref
            
            # If any flash model is available, use first match
            for m in valid_models:
                if "flash" in m:
                    return m
            
            if valid_models:
                return valid_models[0]
    except Exception as e:
        print(f"::notice:: Model discovery endpoint fallback: {e}")
    
    # Fallback to standard 3.8 candidates
    return "models/gemini-3.8-flash"

def call_gemini(api_key: str, diff_text: str) -> str:
    """Calls Gemini REST API with dynamic model discovery and fallback."""
    # Truncate diff to avoid exceeding context window if massive
    max_diff_len = 60000
    if len(diff_text) > max_diff_len:
        diff_text = diff_text[:max_diff_len] + "\n\n... [diff truncated for length] ..."

    prompt = REVIEW_PROMPT.format(diff_text=diff_text)
    
    model_name = discover_model(api_key)
    # Strip leading 'models/' if present for URL formatting
    model_slug = model_name.replace("models/", "")
    
    candidates = [
        f"https://generativelanguage.googleapis.com/v1beta/models/{model_slug}:generateContent?key={api_key}",
        f"https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent?key={api_key}",
        f"https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key={api_key}",
        f"https://generativelanguage.googleapis.com/v1/models/gemini-3.8-flash:generateContent?key={api_key}",
    ]
    
    payload = {
        "contents": [{
            "parts": [{"text": prompt}]
        }],
        "generationConfig": {
            "temperature": 0.2,
            "maxOutputTokens": 2048
        }
    }
    
    last_err = ""
    for url in candidates:
        req = urllib.request.Request(
            url,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json", "User-Agent": "SkyRecompiled-CI/1.0"}
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                response_candidates = data.get("candidates", [])
                if response_candidates:
                    parts = response_candidates[0].get("content", {}).get("parts", [])
                    if parts:
                        return parts[0].get("text", "")
                return "Gemini returned an empty review candidate."
        except urllib.error.HTTPError as e:
            error_body = e.read().decode("utf-8", errors="replace")
            last_err = f"HTTP {e.code}: {error_body}"
            # If 404 (model not found), 503 (high demand), or 429 (rate limited), try next candidate!
            if e.code in (404, 429, 503):
                continue
            # For 400/403, break and raise
            break
        except Exception as e:
            last_err = str(e)
            continue
            
    raise RuntimeError(f"All Gemini API endpoints failed. Last error: {last_err}")

def main():
    api_key = os.environ.get("GEMINI_API_KEY")
    if not api_key:
        print("::warning:: GEMINI_API_KEY not configured. Skipping Gemini automated review.")
        sys.exit(0)

    diff_text = get_git_diff()
    if not diff_text.strip():
        print("No diff detected. Skipping review.")
        sys.exit(0)

    print(f"Generating Gemini review for diff ({len(diff_text)} bytes)...")
    try:
        review = call_gemini(api_key, diff_text)
    except Exception as e:
        print(f"::error title=Gemini Review Failed::{e}")
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_path and os.path.exists(os.path.dirname(summary_path)):
            with open(summary_path, "a", encoding="utf-8") as f:
                f.write(f"### ❌ Gemini Code Review Failed\n\n```\n{e}\n```\n")
        sys.exit(1)
    
    header = "### 🤖 Gemini Code Review\n\n"
    full_review = header + review

    print("\n" + "="*40 + " REVIEW " + "="*40)
    print(full_review)
    print("="*88 + "\n")

    # If running in GitHub Actions, write to Step Summary
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path and os.path.exists(os.path.dirname(summary_path)):
        with open(summary_path, "a", encoding="utf-8") as f:
            f.write(full_review + "\n")

    # If this is a pull request, comment on the PR via gh CLI
    pr_number = os.environ.get("PR_NUMBER")
    if pr_number:
        try:
            subprocess.run(["gh", "pr", "comment", pr_number, "--body", full_review], check=False)
        except Exception:
            pass

    # If running in an Entire-tracked trail, post review to the trail discussion
    try:
        subprocess.run(
            ["entire", "trail", "comment", "add", "--title", "🤖 Gemini Code Review", "--body", full_review],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
    except Exception:
        pass

if __name__ == "__main__":
    main()
