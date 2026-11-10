Block storage
-------------

Block files (`blocks/blk*.dat`) can now be written zstd-compressed, one frame per record, which
saves about a fifth of the space of new block files. Block undo data (`blocks/rev*.dat`) is
compressed the same way, which saves about a quarter.
The new `-blockcompression=<level>` option selects the zstd level.
It defaults to 0 in this release, which stores records exactly as before, so nothing changes
on disk unless the option is set. Level 3 is the recommended setting when opting in; levels above 5
cost several times more CPU for no measurable gain and produce a startup warning. A future release
is planned to enable compression by default.

Files written by earlier versions are read as before and are never rewritten, and builds of this
version with zstd support read compressed records whether or not they write them. That keeps
downgrades simple: a node whose block files contain no compressed records (it never set
`-blockcompression` above 0, and its block files were not copied from a node that did) can be
downgraded to any earlier version without a reindex, and once a later release enables compression
by default, a downgrade to a build of this version with zstd support needs no reindex either. Block
files that contain compressed records need `-reindex` on versions before this one, which cannot
read them, and on builds of this version without zstd support.

zstd is an optional build dependency (`--without-zstd`, `NO_ZSTD=1` for depends). A build without
it stores everything uncompressed, accepts only `-blockcompression=0`, and refuses to start on
block files that contain compressed records. With `-reindex` it skips those records and downloads
the blocks again from peers, reusing their space in the block files.

`contrib/linearize/linearize-data.py` reads compressed records with the `zstandard` Python module
or Python 3.14 and later, and always writes uncompressed records to the bootstrap file.
