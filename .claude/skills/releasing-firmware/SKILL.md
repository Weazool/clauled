---
name: releasing-firmware
description: Use when cutting a Clauled firmware release - bumping the patch, minor, or major version, writing a changelog entry, tagging, or publishing to GitHub Releases
---

# Releasing Firmware

## Overview

Cuts a release so `src/version.h`, the `CHANGELOG.md` entry, the README's latest-release
line, and the git tag can never drift apart. The user confirms the change summary at a
checkpoint before anything is written to disk.

## When to Use

- "bump the version", "cut a release", "release a patch/minor/major"
- After a batch of work is finished, built, and flashed successfully

Not for work in progress, and not for anything that has not compiled.

## The checkpoint is mandatory

Draft the summary, present it, and **stop**. Wait for explicit confirmation.

Nothing may be written before the user confirms — not `version.h`, not the changelog,
not the README, no commit, no tag, no push. If they amend the summary, present the
amended version and wait again.

**No exceptions:**
- Not when the changes "obviously" map to one line
- Not when the user already said "patch" — that is the bump size, not the summary
- Not to save a round trip
- Not when re-running after a failed publish

## Choosing the bump

| Bump | When |
|---|---|
| **major** | Breaking change to the push contract in `API.md`, or to the `secrets.h` format |
| **minor** | New capability, backwards compatible |
| **patch** | Fixes and internal changes only |

If the user did not say which, **ask**. Never infer it from the diff.

## Steps

### 1. Read current state

```bash
grep CLAULED_VERSION src/version.h
git describe --tags --abbrev=0 2>/dev/null || echo "(no tags yet)"
git status --short
```

**Check for uncommitted work.** The release commit must contain the code it claims to
release. If `git status` is not clean, that work is part of this release and gets
committed in step 6 — do not assume only `version.h` and `CHANGELOG.md` need staging.

### 2. Collect what changed

```bash
LAST=$(git describe --tags --abbrev=0 2>/dev/null || git rev-list --max-parents=0 HEAD)
git log "$LAST"..HEAD --oneline
git diff --stat "$LAST"..HEAD
```

If there is uncommitted work, `git log` will not show it. Also run:

```bash
git diff --stat HEAD
git ls-files --others --exclude-standard
```

### 3. Draft the changelog entry

Group under `### Added` / `### Changed` / `### Fixed` / `### Removed`. Describe
observable behaviour, not commit messages. "Push endpoint now rejects bodies over 2 KB"
beats "fix handlePush".

### 4. CHECKPOINT — present and wait

See above. Nothing is written until the user confirms.

### 5. Write, once confirmed

Three files, kept in lockstep:

- `src/version.h` → new version string
- `CHANGELOG.md` → new `## [x.y.z] - YYYY-MM-DD` section directly under `## [Unreleased]`
- `README.md` → update the latest-release line near the top:
  `**Latest release:** vX.Y.Z — see [CHANGELOG.md](CHANGELOG.md).`

Check whether any other doc pins the version — `API.md` carries a `"version"` field in its
`/health` example that should match.

### 6. Commit and tag

Stage everything that belongs to this release, then **verify the secrets file did not get
caught**:

```bash
git add -A
git diff --cached --name-only | grep -x "src/secrets.h" && echo "ABORT: secrets.h staged" || echo "ok"
```

If `src/secrets.h` is staged, stop and fix `.gitignore` before going any further.

Commit with a **bash heredoc**. Do not use PowerShell here-string syntax (`@'...'@`) — in
bash that is a literal `@` and it ends up as the commit subject:

```bash
git commit -q -F - <<'EOF'
Release vX.Y.Z - short summary

Body describing the observable change.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
git tag -a "vX.Y.Z" -m "vX.Y.Z"
```

Verify the subject came out right before continuing:

```bash
git log -1 --format=%s
git rev-list -n1 "vX.Y.Z"; git rev-parse HEAD   # must match
```

If the message is wrong, `git commit --amend` then **delete and recreate the tag** — an
amend changes the SHA and leaves the tag pointing at the old commit.

### 7. Build the artifact

```bash
pio run
```

Do not continue if the build fails. A tagged version that does not compile is worse than
no tag.

### 8. SAFETY GATE — scan before anything goes public

The firmware compiles `secrets.h` in, so a working `.bin` contains the WiFi password in
cleartext. This repo is a **public fork**. Always run:

```bash
BIN=.pio/build/esp32-c3-mini/firmware.bin
LEAK=0
for k in WIFI_SSID WIFI_PASSWORD CLAULED_PUSH_KEY; do
  v=$(sed -n "s/^#define $k *\"\(.*\)\".*/\1/p" src/secrets.h)
  [ -z "$v" ] && continue
  if grep -aqF -- "$v" "$BIN"; then echo "LEAK: $k found in $BIN"; LEAK=1; fi
done
[ "$LEAK" -eq 0 ] && echo "clean - safe to attach"
```

Also confirm no credential value reached a tracked file:

```bash
for v in $(sed -n 's/^#define \(WIFI_SSID\|WIFI_PASSWORD\|CLAULED_PUSH_KEY\) *"\(.*\)".*/\2/p' src/secrets.h); do
  git grep -qF -- "$v" HEAD && echo "LEAK in tracked files: $v"
done
```

**If anything leaks, do not attach the binary.** Report it and offer:
- publish a **notes-only** release (the correct answer for a public repo), or
- rebuild with placeholder credentials first — note the image is then non-functional until
  reflashed, so it is of limited use to anyone

Never attach a binary that failed this scan, even if asked to "just ship it" — publishing a
credential is irreversible and gets indexed within seconds. Offer notes-only instead.

### 9. Publish

**Confirm with the user first.** This is public and irreversible. State exactly what will
be pushed and whether a binary is attached.

```bash
git push origin main --follow-tags
gh release create "vX.Y.Z" --title "vX.Y.Z" --notes "<changelog section>"
```

Append the binary path **only** if step 8 came back clean.

## Common Mistakes

| Mistake | Fix |
|---|---|
| Writing files before the checkpoint | Revert them, re-present, wait |
| Staging only `version.h` and `CHANGELOG.md` | If there is uncommitted work, the tag points at a commit without the code. Stage everything. |
| PowerShell here-string in a bash commit | Use `git commit -F -` with a `<<'EOF'` heredoc |
| Amending without moving the tag | Amend changes the SHA — delete and recreate the tag |
| Inferring the bump size from the diff | Ask |
| Tagging before the build passes | Build first; a bad tag must be deleted on the remote too |
| Attaching a binary without scanning | Run the gate every time, including on re-runs |
| Forgetting the README line | `version.h`, `CHANGELOG.md`, and `README.md` move together |
