#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
'''
Check that executables do not link Rust code.

The Dash Platform CXX bindings (--enable-platform-gui) are for dash-qt only;
dashd, the command-line tools and the fuzz binary must not contain any of it.
A binary fails if its symbol table has cxx bridge, Rust runtime or Rust
standard library symbols, or has no symbols at all (a stripped binary cannot
be checked).

Example usage:

    contrib/devtools/check-no-rust.py src/dashd src/dash-cli src/dash-tx src/dash-wallet src/test/fuzz/fuzz
'''
import re
import subprocess
import sys

from utils import determine_wellknown_cmd

# cxx bridge symbols carry "cxxbridge1$". Rust code is either legacy-mangled
# under a crate namespace (rust, std, core, alloc) or v0-mangled ("_R"
# followed by a path tag and a crate root "Cs<disambiguator>_"), which C and
# C++ symbols never match. The allocator shim, the panic handler and the
# unwinding personality are plain C names every Rust program links.
RUST_SYMBOL = re.compile(
    r'cxxbridge1\$'
    r'|_ZN(4rust|3std|4core|5alloc)[0-9]'
    r'|\b_?_R[A-Za-z0-9_]*?Cs[A-Za-z0-9]*_[0-9]'
    r'|\b_?(__rust_alloc|rust_begin_unwind|rust_eh_personality)'
)


def rust_symbols(nm, path):
    result = subprocess.run(nm + [path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    if result.returncode != 0:
        sys.exit(f'{path}: {result.stderr.strip()}')
    lines = result.stdout.splitlines()
    if not lines:
        sys.exit(f'{path}: no symbols, so it cannot be checked (stripped?)')
    return [line for line in lines if RUST_SYMBOL.search(line)]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    nm = determine_wellknown_cmd('NM', 'nm')
    failed = False
    for path in sys.argv[1:]:
        found = rust_symbols(nm, path)
        if found:
            failed = True
            print(f'{path}: links Rust code, for example:')
            for line in found[:5]:
                print(f'    {line}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
