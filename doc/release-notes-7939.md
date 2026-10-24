Block storage
-------------

Block files (`blocks/blk*.dat`) and undo files (`blocks/rev*.dat`) can now be written
zstd-compressed, one frame per record, which saves about a fifth of the space of new block files
and about a quarter of new undo files. The new `-blockcompression=<level>` option selects the zstd
level. It defaults to 0 in this release, which stores records exactly as before, so nothing changes
on disk unless the option is set. Level 3 is the recommended setting when opting in; levels above 5
cost several times more CPU for no measurable gain and produce a startup warning. A future release
is planned to enable compression by default.

Files written by earlier versions are read as before and are never rewritten, and this version
reads compressed records whether or not it writes them. That keeps downgrades simple: a node that
never set `-blockcompression` can be downgraded to any earlier version without a reindex, and once
a later release enables compression by default, a downgrade to this version needs no reindex
either. Only a downgrade to a version before this one after compression has been used requires
`-reindex`, since older versions cannot read the compressed records.

zstd is an optional build dependency (`--without-zstd`, `NO_ZSTD=1` for depends). A build without
it stores everything uncompressed, accepts only `-blockcompression=0`, and refuses to start on
block files written with compression until `-reindex` re-downloads those blocks.

`contrib/linearize/linearize-data.py` reads compressed records with the `zstandard` Python module
or Python 3.14 and later, and always writes uncompressed records to the bootstrap file.
