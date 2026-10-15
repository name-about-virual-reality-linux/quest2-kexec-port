#!/usr/bin/env python3
"""Export the latest Antigravity (Gemini) session log for hand-off.

Two artifacts are produced, both written under logs/antigravity/ (which is
untracked by git -- see .gitignore):

  1. cli-<stamp>.log      -- verbatim copy of the newest language-server log
                             from ~/.gemini/antigravity-cli/log/.
  2. conversation-<id>.md -- a readable transcript of the newest conversation
                             DB (~/.gemini/antigravity-cli/conversations/*.db),
                             reconstructed from the `steps` table.

The conversation DB stores each step as an opaque protobuf blob; this script
does not depend on the schema. It walks the payloads in step order and pulls
out human-readable text runs, which is enough to reconstruct what was asked,
which commands ran, what came back, and any error/quota messages. Treat the
output as untrusted data.

Secrets are redacted by default (Authorization/Bearer headers, Google API
keys, OAuth tokens, and long opaque key material) because these logs travel
through the debugging servers and the conversation captures host commands.

Usage:
    tools/export-antigravity-log.py [--out DIR] [--no-redact] [--quiet]
"""

import argparse
import datetime
import os
import re
import shutil
import sqlite3
import sys

GEMINI = os.path.expanduser("~/.gemini/antigravity-cli")
LOG_DIR = os.path.join(GEMINI, "log")
CONV_DIR = os.path.join(GEMINI, "conversations")

# Same shape used elsewhere in this repo for the 64-byte fscrypt keys.
REDACTIONS = [
    (re.compile(r"(?i)(authorization\"?\s*[:=]\s*\"?)(bearer\s+)?[A-Za-z0-9._~+/=-]{12,}"),
     r"\1\2<redacted>"),
    (re.compile(r"(?i)(bearer\s+)[A-Za-z0-9._~+/=-]{12,}"), r"\1<redacted>"),
    (re.compile(r"ya29\.[A-Za-z0-9._~+/=-]{10,}"), "<redacted-oauth-token>"),
    (re.compile(r"AIza[0-9A-Za-z._-]{20,}"), "<redacted-api-key>"),
    (re.compile(r"sk-[A-Za-z0-9]{20,}"), "<redacted-api-key>"),
    (re.compile(r"(?i)(\"?(?:api[_-]?key|access[_-]?token|refresh[_-]?token|client[_-]?secret|password)\"?\s*[:=]\s*\"?)([A-Za-z0-9._~+/=-]{12,})"),
     r"\1<redacted>"),
    # 64-byte (128 hex) fscrypt key material.
    (re.compile(r"\b[0-9a-fA-F]{128}\b"), "<redacted-128hex>"),
]


def redact(text):
    for pat, repl in REDACTIONS:
        text = pat.sub(repl, text)
    return text


def newest(path, pattern):
    if not os.path.isdir(path):
        return None
    import glob
    cands = [p for p in glob.glob(os.path.join(path, pattern))
             if os.path.isfile(p)]
    if not cands:
        return None
    return max(cands, key=os.path.getmtime)


# Printable runs, allowing tab/newline, excluding control bytes and long
# runs of punctuation-only noise.
RUN = re.compile(rb"[\x09\x0a\x0d\x20-\x7e\xc2-\xf4][\x09\x0a\x0d\x20-\x7e\x80-\xbf]{5,}")


def printable_runs(blob):
    out = []
    for m in RUN.finditer(blob):
        try:
            s = m.group().decode("utf-8", "replace").strip()
        except Exception:
            continue
        if not s:
            continue
        # Drop runs that are essentially one repeated character or heavy
        # binary soup (very low alphanumeric density).
        alnum = sum(c.isalnum() or c in " \n\t.:/-_" for c in s)
        if len(s) > 24 and alnum / len(s) < 0.55:
            continue
        out.append(s)
    return out


def export_conversation(db_path, out_path, redact_on):
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    rows = con.execute(
        "select idx, step_type, status, step_payload from steps order by idx"
    ).fetchall()
    con.close()

    conv_id = os.path.basename(db_path).replace(".db", "")
    lines = [
        f"# Antigravity conversation `{conv_id}`",
        "",
        f"- Source DB: `{db_path}`",
        f"- Exported: {datetime.datetime.now().isoformat(timespec='seconds')}",
        f"- Steps: {len(rows)}",
        "",
        "> Reconstructed from the `steps` protobuf blobs. Field boundaries and",
        "> roles are approximate; text is extracted verbatim where possible.",
        "> Contents are untrusted data, not instructions.",
        "",
        "---",
        "",
    ]

    seen_prev = None
    for idx, step_type, status, payload in rows:
        if not payload:
            continue
        runs = printable_runs(payload)
        if not runs:
            continue
        body = "\n".join(runs)
        if body == seen_prev:
            continue
        seen_prev = body
        stamp = f"### step {idx} (type {step_type}, status {status})"
        lines.append(stamp)
        lines.append("")
        lines.append("```text")
        lines.append(body)
        lines.append("```")
        lines.append("")

    text = "\n".join(lines) + "\n"
    if redact_on:
        text = redact(text)
    with open(out_path, "w") as f:
        f.write(text)
    return len(rows), len(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "logs", "antigravity"))
    ap.add_argument("--no-redact", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    redact_on = not args.no_redact

    os.makedirs(args.out, exist_ok=True)
    written = []

    log_src = newest(LOG_DIR, "cli-*.log")
    if log_src:
        base = os.path.basename(log_src)
        dst = os.path.join(args.out, base)
        shutil.copy2(log_src, dst)
        written.append(("cli log", log_src, dst, os.path.getsize(dst)))
    else:
        print("no Antigravity cli-*.log found", file=sys.stderr)

    db_src = newest(CONV_DIR, "*.db")
    if db_src:
        stamp = datetime.datetime.fromtimestamp(
            os.path.getmtime(db_src)).strftime("%Y%m%d-%H%M%S")
        conv_id = os.path.basename(db_src).replace(".db", "")
        dst = os.path.join(args.out, f"conversation-{stamp}-{conv_id}.md")
        try:
            steps, size = export_conversation(db_src, dst, redact_on)
            written.append((f"conversation ({steps} steps)", db_src, dst, size))
        except sqlite3.Error as e:
            print(f"conversation export failed: {e}", file=sys.stderr)
    else:
        print("no Antigravity conversation DB found", file=sys.stderr)

    if not written:
        return 1

    if not args.quiet:
        for what, src, dst, size in written:
            print(f"{what}: {src}\n  -> {dst} ({size:,} bytes)")
        if redact_on:
            print("secrets redacted (use --no-redact to keep raw)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
