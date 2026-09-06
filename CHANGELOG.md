# Changelog

All notable Astonia Uncharted changes are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
using Added, Changed, Fixed, Removed, Security, and Migration categories.

## [Unreleased]

### Fixed

- Enable large-file filesystem interfaces for the 32-bit Linux build so WSL
  Windows-mounted zone directories with large inode numbers load correctly.
- Stop startup with an explicit error when zone directory enumeration fails,
  instead of silently starting an empty world.

### Added

- Project governance and development documentation for the Server 3 baseline.

## [0.1.0] - 2026-09-06

### Added

- Established Astonia Uncharted from the Astonia Community Server 3 baseline.
