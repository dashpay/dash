Block storage
-------------

New blocks are written to `blocks/blk*.dat` zstd-compressed, one frame per block, which saves
about a fifth of the space new block files take. Block files written by earlier versions are read
as before and are not rewritten. The new `-blockcompression=<level>` option selects the zstd level
(default 0, stores blocks uncompressed for backwards compatibility).

zstd is an optional build dependency (`--without-zstd`, `NO_ZSTD=1` for depends). A build without
it stores everything uncompressed, accepts only `-blockcompression=0`, and refuses to start on
block files written with compression until `-reindex` re-downloads those blocks.

Downgrading to a version without this feature requires a `-reindex` if blockcompression > 0 had ever
been used, since older versions cannot read the compressed records.
`contrib/linearize/linearize-data.py` needs the `zstandard` Python module (or Python 3.14+) to read
block files containing compressed records.
