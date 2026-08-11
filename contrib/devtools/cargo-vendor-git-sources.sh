#!/bin/sh
export LC_ALL=C

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Emit cargo source-replacement stanzas for every git source in a Cargo.lock,
# for appending to an offline .cargo/config.toml whose vendored-sources
# directory holds the output of `cargo vendor`. `cargo vendor` prints these
# stanzas itself, but only at vendor time; deriving them from the lockfile
# lets the build system reconstruct the config without re-running vendor.
#
# usage: cargo-vendor-git-sources.sh path/to/Cargo.lock

set -eu

if [ "$#" -ne 1 ] || [ ! -f "$1" ]; then
    echo "usage: $0 path/to/Cargo.lock" >&2
    exit 1
fi

# Lockfile entries look like:
#   source = "git+https://github.com/dashpay/platform?tag=v4.1.0#bfc80249..."
# The stanza key is the source without the fragment; the query parameter
# (tag=/branch=/rev=), when present, becomes a key of the same name.
grep '^source = "git+' "$1" | sed 's/^source = "//; s/"$//; s/#.*//' | sort -u | \
while IFS= read -r src; do
    url_query="${src#git+}"
    url="${url_query%%\?*}"
    printf '\n[source."%s"]\ngit = "%s"\n' "$src" "$url"
    case "$url_query" in
        *\?*)
            query="${url_query#*\?}"
            printf '%s = "%s"\n' "${query%%=*}" "${query#*=}"
            ;;
    esac
    printf 'replace-with = "vendored-sources"\n'
done
