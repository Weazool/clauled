// Single source of truth for the firmware version.
//
// Bumped by the `releasing-firmware` skill, which keeps this file, the
// CHANGELOG entry, and the git tag in lockstep. Don't edit by hand unless
// you are also fixing the tag.
//
// Reported at GET /health and on the boot screen, so a device can always
// tell you what it is actually running.

#pragma once

#define CLAULED_VERSION "4.0.0"
