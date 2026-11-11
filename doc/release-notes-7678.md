Build System
------------

- BLS signatures are now computed with [blst](https://github.com/supranational/blst) instead of relic. Unlike relic, blst is not part of the Dash Core source tree, so builds without `depends` need blst 0.3.17 installed. Some distributions do not package it; see the blst section of `doc/build-unix.md` for building it from source. (#7678)
