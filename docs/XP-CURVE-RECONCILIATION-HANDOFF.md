# XP curve reconciliation handoff

## Goal

Prepare the existing 2x leveling XP change as one understandable, verified,
reviewable feature branch. This is organization and verification of an
experiment, not a request to redesign the curve. The owner wants military
ranks handled later as a separate change.

## Checkout and branch

- Work in this VS Code window's checkout: `C:\Astonia\35server\Astonia-Uncharted`.
- Current branch: `docs/xp-curve-reconciliation`, based on `main`.
- The owner's older, mixed experiment checkout is
  `C:\Astonia\Astonia Uncharted\AU 3.0S\astonia_community_server3-main`.
  Do not edit or bulk-copy from it; its `tool.c` contains unrelated changes.
- The owner has created a GitHub issue titled
  `Reconcile and isolate the 2× leveling XP curve` (issue number/link not
  provided yet).

## Current state

- `tool.c` in this clean checkout has an unstaged, user-made edit to exactly
  three functions: `exp2level`, `level2exp`, and `level_value`.
- The intended formulas are `max(1, (int)sqrt(sqrt(val / 2.0)))`,
  `2 * pow(level, 4)`, and
  `level2exp(level + 1) - level2exp(level)`.
- No files are staged; nothing has been committed or pushed.
- This handoff file is new documentation. Preserve the user's existing
  `tool.c` edit.

## Intended XP scope to verify

- XP thresholds use `2 * L^4`, with level cap 160.
- Server inverse and per-level XP reward calculation agree with those
  thresholds.
- Client XP display uses the same curve.
- Skill-training costs remain unchanged.
- Existing-character XP migration and other reward balancing must be checked
  against the owner's decisions and documented separately; do not infer or
  perform a migration from the formula edit.
- Build and run the appropriate focused checks; record actual commands and
  results. Do not claim runtime testing unless it was actually done.

## Important context and guardrails

- The owner says this is a creative local project and there is no live server.
  Older project notes may describe deployments; treat those as historical
  records to reconcile, not evidence of a currently running server.
- The older checkout is an experiment/archive workspace. The clean clone is
  where one selected change is assembled, checked, committed, and later shared.
- A branch push makes the work visible on GitHub but does not merge it to
  `main`. Do not commit, push, merge, deploy, or edit the older checkout without
  the owner's explicit direction.
- Explain the next action briefly and proceed one step at a time; the owner is
  new to Git and wants concise guidance.

## Next actions

1. Inspect the relevant server and client implementations in this checkout and
   compare them with the intended curve; do not copy whole mixed files.
2. Identify every XP-curve-dependent change needed, including the level-160
   cap and client display, and keep unrelated features (especially military
   ranks) out of this branch.
3. Ask the owner before making any behavioral decision not already specified.
4. Run focused verification, update the issue/docs with evidence, then show the
   complete diff for review before committing or pushing.
