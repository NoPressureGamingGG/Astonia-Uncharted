# XP Curve Reconciliation Handoff

## Summary

The Server 3 progression curve is a cumulative fourth-power model:

- level2exp(level) = level^4
- level_value(level) = (level + 1)^4 - level^4
- exp2level(exp) = floor_fourth_root(exp)

This is the core curve the game uses for level thresholds, XP rewards, and level-up checks. The implementation in `tool.c` was relying on floating-point `sqrt(sqrt(...))`, which can drift around exact quartic boundaries and produce off-by-one level results near threshold values. That drift is especially visible whenever a character is right at a level cutoff or calculating how much XP remains until the next level.

## Reconciliation target

Keep the curve exact and integer-safe:

- `exp2level(level2exp(level)) == level` for all valid level numbers
- `level2exp(level + 1) - level2exp(level) == level_value(level)`
- The function never produces a negative or floating-point threshold result
- Low-level XP gains continue to trigger the same save and profession logic during `check_levelup()`

## Code path affected

- `tool.c`: `exp2level()`, `level2exp()`, `level_value()`, and `check_levelup()`
- `tool.h`: exported XP/level helper declarations

## Implementation note

The fix replaces the float-based inverse root with a bounded integer square-root routine and exact integer arithmetic. The threshold comparison uses the same polynomial identity as the level-to-XP formula instead of relying on floating-point rounding, keeping all level boundaries aligned with the server’s existing progression model.

## Verification

Validate with a targeted build of the affected object and a small set of boundary checks around the exact perfect-fourth-power values:

- 1, 16, 81, 256, 625, 1296, ...
- verifying the level threshold round-trips and next-level XP deltas

This change is intentionally limited to the progression curve and does not alter the broader Server 35 compatibility plan or the separate 64-bit modernization track.
