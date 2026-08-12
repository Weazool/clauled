# Changelog

All notable changes to this project are documented here.

Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **major** — breaking change to the push API or the secrets format
- **minor** — new capability, backwards compatible
- **patch** — fixes and internal changes only

## [Unreleased]

## [1.0.0] - 2026-08-12

First version of the push-based architecture. Forked from
[rafbanaan/clauled](https://github.com/rafbanaan/clauled) and reworked so the
device holds no Claude credentials.

### Added
- `POST /push` ingest endpoint, authenticated with a shared `X-Clauled-Key`
  header compared in constant time.
- `GET /health` for reachability checks — exposes uptime, version, and last-push
  age, and nothing sensitive.
- mDNS advertisement, so the device is reachable at `clauled.local`.
- Compile-time configuration in a gitignored `src/secrets.h`, with a tracked
  `src/secrets.h.example` template.
- `static_assert` guards that turn missing configuration into a build error
  with an actionable message.
- Event pages, with word wrapping and a TTL, ready for notification pushes.
- Staleness tracking in the footer, shown as information rather than an error.
- Graceful degradation when the OLED is absent: the device logs the failure,
  reports `display_ok: false` from `/health`, and carries on serving pushes.
  Previously a missing display halted boot in an infinite loop, taking WiFi and
  the endpoint down with it.
- `API.md` documenting the push contract.

### Removed
- The Claude OAuth token and everything that used it — the device no longer
  contacts `api.anthropic.com` or `console.anthropic.com` at all.
- The OAuth refresh flow.
- The first-boot WiFi setup portal, which ran an **open** access point serving
  a plaintext HTTP form that you typed your token into.
- The unauthenticated browser config page and all of its write endpoints,
  along with `data/index.html` and its screenshots.
- LittleFS and the `uploadfs` flashing step — no filesystem image is needed.
- NTP and all wall-clock handling. Reset countdowns now arrive as seconds
  remaining and tick locally.

### Fixed
- README referenced a `Wire.setClock(400000)` call that did not exist in the
  source.
