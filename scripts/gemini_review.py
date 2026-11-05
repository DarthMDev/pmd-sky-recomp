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

REVIEW_PROMPT = """You are a Principal Software Engineer performing an automated code review on SkyRecompiled (an AOT static recompilation PC port of Pokémon Mystery Dungeon: Explorers of Sky).

Project Context & Strict Invariants:
1. Clean-Room Discipline: Under NO circumstances may copyrighted Nintendo/Chunsoft ROM binaries (.nds), extracted proprietary sprites/soundfonts, or proprietary script dumps be committed.
2. Save Safety: EEPROM save format (131072 bytes, 128KB), 4-page live validation cap, non-volatile state integrity.
3. Modern Architecture: C++20 standard, BIOS-free direct boot, zero undefined behavior, multi-platform compatibility (macOS/Linux/Windows).
4. Testing: New tools or core logic must have automated unit tests.

Review the following git diff. Provide a clear, actionable review:
- **Summary**: 2-3 sentences summarizing the changes.
- **Clean-Room & Compliance**: Confirm whether assets or copyrighted material are clean.
- **Code Quality & Architecture**: Spot any memory leaks, logic bugs, thread-safety issues, or missed edge cases.
- **Recommendations**: Concrete, actionable suggestions (or state 'LGTM' if ready to merge).

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

def call_gemini(api_key: str, diff_text: str) -> str:
    """Calls Gemini REST API using urllib."""
    # Truncate diff to avoid exceeding context window if massive
    max_diff_len = 60000
    if len(diff_text) > max_diff_len:
        diff_text = diff_text[:max_diff_len] + "\n\n... [diff truncated for length] ..."

    prompt = REVIEW_PROMPT.format(diff_text=diff_text)
    
    # Try gemini-1.5-flash first (fast, generous free tier rate limits)
    url = f"https://generativelanguage.googleapis.com/v1beta/models/gemini-1.5-flash:generateContent?key={api_key}"
    payload = {
        "contents": [{
            "parts": [{"text": prompt}]
        }],
        "generationConfig": {
            "temperature": 0.2,
            "maxOutputTokens": 2048
        }
    }
    
    req = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"}
    )
    
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            candidates = data.get("candidates", [])
            if candidates:
                parts = candidates[0].get("content", {}).get("parts", [])
                if parts:
                    return parts[0].get("text", "")
            return "Gemini returned no review content."
    except urllib.error.HTTPError as e:
        error_body = e.read().decode("utf-8", errors="replace")
        return f"Gemini API Error (HTTP {e.code}): {error_body}"
    except Exception as e:
        return f"Gemini API Request failed: {e}"

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
    review = call_gemini(api_key, diff_text)
    
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

if __name__ == "__main__":
    main()
