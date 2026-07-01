# Changelog

## [Unreleased]

### Bug Fixes

- Fixed bug where column families would be non-atomically flushed when one memtable was filled, resulting in inconsistency after a crash.

[Unreleased]: https://github.com/nomic-io/merk/compare/v1.0.0-alpha.8...HEAD
