# Collaboration Workflow

## Source of truth

GitHub Issues define work. Branches and pull requests contain implementation.
`AGENTS.md`, the roadmap, decisions, changelog, and mechanics map provide the
shared context that every human or agent must read before starting work.

## One issue, one owner, one branch

Each issue has one current owner and one branch. Parallel research is welcome,
but two agents must not edit the same file or mechanic without an explicit
division of responsibilities.

Use branch names such as:

- `research/mechanics-map`
- `feature/<short-purpose>`
- `fix/<short-problem>`
- `docs/<short-topic>`

## Required handoff

Every handoff, pull request, or agent report states:

1. The issue and its player-facing purpose.
2. Files and mechanics examined or changed.
3. Verification performed and its result.
4. Questions requiring gameplay-owner input.
5. Risks, compatibility notes, and follow-up work.

## Review and merge

The project owner decides intended player experience. A change is ready to
merge only after its gameplay purpose, verification, changelog impact, and
rollback path are clear. The coordinator resolves conflicts between agents and
records material decisions in `docs/DECISIONS.md`.
