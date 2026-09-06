# Architecture and Design Decisions

## 2026-09-06 — Server 3 is the primary baseline

**Decision:** Start Astonia Uncharted on Astonia Community Server 3.

**Reason:** It matches the desired legacy mechanics and the downloaded 3.0S
environment. Server 35 will be evaluated later as a port target, not blended
into the first implementation.

**Consequences:** Development, testing, and initial documentation target Server
3. Any future Server 35 work requires an explicit comparison and migration plan.

## 2026-09-06 — PathOfAstonia is reference-only

**Decision:** Do not import PathOfAstonia into this repository.

**Reason:** It is prior work with independent history. Ideas may be examined and
reimplemented deliberately, with provenance recorded.
