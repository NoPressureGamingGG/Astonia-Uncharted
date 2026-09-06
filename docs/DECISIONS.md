# Architecture and Design Decisions

## 2026-09-06 — Use large-file interfaces before a full 64-bit port

**Decision:** Keep `-m32` and enable `_FILE_OFFSET_BITS=64` consistently through
the shared compiler flags. Detect directory enumeration errors in the zone loader.

**Evidence:** On the WSL `/mnt/c` checkout, the original 32-bit `readdir` probe
returned zero entries and `EOVERFLOW` (75). The same probe with large-file
interfaces returned 50 entries successfully. The running server had reported
zero character/item templates and rejected Unknown's first login because
`new_mage_m` was not loaded, although its definition existed on disk.

**Scope:** This changes filesystem interfaces, not pointer width or gameplay
structure layout. A full 64-bit conversion remains a separate sequence requiring
audits of pointer casts, serialized character/item data, drivers and protocol
assumptions. No Server 35 mechanics are introduced.

**Deployment and rollback:** Force a full rebuild (`make -B -j4`) because Make
does not track compiler flag changes. Preserve existing executables, objects and
runtime drivers before building. Stop affected development processes before
replacing drivers in use. To roll back, remove the added compiler define and
zone enumeration error check, rebuild all targets, and restart the development
server; alternatively restore the preserved matching executables and drivers.
No database schema or stored character data migration is part of this change.

**Verification:** `make -B -j4` completed in an isolated `/tmp` checkout. The
result is still ELF i386. After deployment, Cameron loaded 139 character and
1,249 item templates, then populated 292 characters and 1,702 items. All 30
development area processes were restarted. `git diff --check` passed. The owner
subsequently confirmed a successful real player login; existing missing-clan-
storage warnings are separate from this filesystem fix.

**Local recovery artifacts:** Original `server`, `runtime.tar`, the full build
log, and recorded area launch arguments are preserved under ignored
`.obj/largefile-rollback-20260906-112941/`. Restore only with these checkout's
area processes stopped; keep the executable and drivers together. The new
build copy is `/tmp/au-largefile-build-8XFjeV`. Ports are allocated dynamically:
Cameron selected 5556 after this restart, so clients must not assume the earlier
5557 assignment still identifies Cameron.

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
