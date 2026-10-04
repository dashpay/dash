Updated RPCs
------------

- `scanblocks` now returns an error instead of a partial result when block filters for the requested range are not available. This happens while the block filter index is still being built (for example right after enabling `-blockfilterindex` on an existing node) or when a filter cannot be read from disk. Previously such ranges were skipped silently and the result still reported `to_height` as the end of the requested range. (#7799)
