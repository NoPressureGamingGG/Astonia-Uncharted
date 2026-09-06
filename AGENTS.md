# Astonia Uncharted — Agent Guide

## Project intent

This is Astonia Uncharted’s Server 3 line: a significant, creative, and
purposeful reimagining of the game. We make large changes slowly, learning the
existing systems first and reusing their strongest pieces where useful. Every
change must have a clear design "why".

Server 35 is a future porting and evaluation target, not code to merge into
this branch. A future 64-bit modernization is expected, but is separate from
the first gameplay work.

## Repository roles

- `origin`: `NoPressureGamingGG/Astonia-Uncharted` — Uncharted work and releases.
- `upstream`: `AstoniaCommunity/astonia_community_server3` — read-only upstream.
- `PathOfAstonia`: reference only; do not copy from it without an explicit task.

## Working rules

1. Explore boldly, but keep each implementation step focused and testable.
2. Ask the project owner when gameplay behavior or player experience is unclear;
   their long-term player knowledge is primary evidence.
3. Do not mix Server 35 mechanics into Server 3; record porting ideas in docs.
4. Update `CHANGELOG.md` for player-visible or operational changes.
5. Record directional, compatibility, data, or deployment decisions in
   `docs/DECISIONS.md`.
6. Never commit credentials, database dumps, generated binaries, runtime logs,
   or production configuration.
7. Before each push, run `git diff --check`, build, and describe non-obvious
   verification in the commit body.

## Commit format

Use focused imperative subjects, for example `area: rebalance zombie reward` or
`build: document WSL prerequisites`. Do not combine formatting, refactors, and
gameplay changes in one commit.
