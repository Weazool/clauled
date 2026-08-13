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

Since v2.0.0 the device holds **no credentials**, so a release binary should contain
nothing sensitive by construction. The gate stays because "should" is not "does" — this
repo is a **public fork**, and publishing a credential is irreversible and indexed within
seconds.

**Match on VALUES, never on names.** A substring gate keyed on words like `WIFI_PASS` or
`sk-ant` fires on `ESP_ERR_WIFI_PASSWORD` in the ESP-IDF error table and on the
`sk-ant-oat01-...` placeholder in a README. Both have happened. A gate that cries wolf
gets waved through, which is worse than no gate.

```bash
BIN=.pio/build/esp32-c3-mini/firmware.bin
node -e '
const fs=require("fs"),os=require("os"),p=require("path");
const bin=fs.readFileSync(process.argv[1]);
let leak=0;
// Real values from the local config, never printed - only whether they appear.
try {
  const cfg=JSON.parse(fs.readFileSync(p.join(os.homedir(),".clauled.json"),"utf8"));
  for (const k of ["token"]) {
    const v=cfg[k];
    if (v && v.length>20 && bin.includes(Buffer.from(v))) { console.log("LEAK: "+k); leak=1; }
  }
} catch {}
console.log(leak ? "DO NOT ATTACH" : "clean - safe to attach");
' "$BIN"
```

If `src/secrets.h` ever comes back, add its values to the same value-based check. Do not
reintroduce a name-based one.

**If anything leaks, do not attach the binary.** Report it and publish a notes-only
release instead — the correct answer for a public repo. Never attach a binary that failed
this scan, even if asked to "just ship it".


### 9. Publish

**Confirm with the user first.** This is public and irreversible. State exactly what will
be pushed and whether a binary is attached.

```bash
git push origin main --follow-tags
```

**Always pass `--repo`.** This repo is a fork with an `upstream` remote, and `gh`
resolves to the **parent** (`rafbanaan/clauled`) by default — without `--repo` it tries
to release against someone else's repository:

```bash
awk '/^## \[X\.Y\.Z\]/{f=1;next} f && /^## \[/{exit} f' CHANGELOG.md > /tmp/notes.md
gh release create "vX.Y.Z" --repo Weazool/clauled --title "vX.Y.Z" --notes-file /tmp/notes.md
```

Use `--notes-file`, not `--notes` — changelog text contains backticks, quotes and
newlines that get mangled when inlined as a shell argument.

Append the binary path **only** if step 8 came back clean. Then verify what actually
shipped:

```bash
gh release view "vX.Y.Z" --repo Weazool/clauled --json assets -q '.assets[].name'
```

Empty output confirms a notes-only release with no binary attached.

### 10. Refresh the marketplace clone (plugin releases only)

**Pushing a plugin release does not make it visible to Claude Code.** The app
resolves updates against a git clone on disk, not against GitHub — so the new
version is offered only after that clone is refreshed. Skipping this produces a
release that exists on GitHub while the UI insists there is no update:

```bash
git -C ~/.claude/plugins/marketplaces/weazool-clauled pull --ff-only
```

Then confirm the clone advertises the new version:

```bash
node -e "console.log(JSON.parse(require('fs').readFileSync(process.env.USERPROFILE+'/.claude/plugins/marketplaces/weazool-clauled/.claude-plugin/plugin.json','utf8')).version)"
```

Note there are **two copies of a plugin on disk** and they serve different
purposes:

| Copy | Path | Used for |
|---|---|---|
| marketplace clone | `plugins/marketplaces/<name>/` | update discovery, and any path referenced directly from `settings.json` |
| installed cache | `plugins/cache/<market>/<plugin>/<version>/` | **hooks load from here** |

Editing the clone does not change what hooks run. After updating, verify the
registry actually moved:

```bash
node -e "const j=require(process.env.USERPROFILE+'/.claude/plugins/installed_plugins.json');console.log(JSON.stringify(j.plugins,null,1))"
```

Hooks are registered at **app startup**, so a plugin update needs a restart of
Claude Code — not merely a new chat.

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
| Pushing before reading the gate output | Read it first. Getting the answer you wanted is not the same as verifying it. |
| A gate that matches on names, not values | `WIFI_PASS` hits `ESP_ERR_WIFI_PASSWORD`; `sk-ant` hits a README placeholder. Match real values only. |
| Leaving the README's latest-release line behind | It sat at v3.0.0 for the whole of v3.0.1. Check it every time. |
| Forgetting the README line | `version.h`, `CHANGELOG.md`, and `README.md` move together |
| `gh release create` without `--repo` | On a fork, `gh` targets the upstream parent, not yours |
| Inlining notes with `--notes` | Backticks and quotes get mangled; use `--notes-file` |
