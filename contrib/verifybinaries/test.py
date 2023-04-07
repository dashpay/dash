#!/usr/bin/env python3

import json
import sys
import subprocess
from pathlib import Path


def main():
    """Tests ordered roughly from faster to slower."""
    expect_code(run_verify("", "pub", '0.32'), 4, "Nonexistent version should fail")
    expect_code(run_verify("", "pub", '0.32.awefa.12f9h'), 11, "Malformed version should fail")
    expect_code(run_verify('--min-good-sigs 20', "pub", "24.0.2"), 9, "--min-good-sigs 20 should fail")

    print("- testing multisig verification (24.0.2)", flush=True)
    _2402 = run_verify("--json", "pub", "24.0.2")
    try:
        result = json.loads(_2402.stdout.decode())
    except Exception:
        print("failed on 24.0.2 --json:")
        print_process_failure(_2402)
        raise

    expect_code(_2402, 0, "24.0.2 should succeed")
    v = result['verified_binaries']
    assert result['good_trusted_sigs']
    assert v['dashcore-24.0.2-aarch64-linux-gnu.tar.gz'] == '26184fc0925508be207ace7462aaf5f434697459c7c84ca16250e3e879839fc2'
    assert v['dashcore-24.0.2-x86_64-apple-darwin.tar.gz'] == '3139fdb1f5cc421c4de2c759d0736ced904a648d060cd71bc28dd1fac5932560'
    assert v['dashcore-24.0.2-x86_64-linux-gnu.tar.gz'] == '347e1c49ec2d37c1e5544ee7e16c91fda9d7ef007a62036130c0924c867dcc84'


def run_verify(global_args: str, command: str, command_args: str) -> subprocess.CompletedProcess:
    maybe_here = Path.cwd() / 'verify.py'
    path = maybe_here if maybe_here.exists() else Path.cwd() / 'contrib' / 'verifybinaries' / 'verify.py'

    if command == "pub":
        command += " --cleanup"

    return subprocess.run(
        f"{path} {global_args} {command} {command_args}",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, shell=True)


def expect_code(completed: subprocess.CompletedProcess, expected_code: int, msg: str):
    if completed.returncode != expected_code:
        print(f"{msg!r} failed: got code {completed.returncode}, expected {expected_code}")
        print_process_failure(completed)
        sys.exit(1)
    else:
        print(f"✓ {msg!r} passed")


def print_process_failure(completed: subprocess.CompletedProcess):
    print(f"stdout:\n{completed.stdout.decode()}")
    print(f"stderr:\n{completed.stderr.decode()}")


if __name__ == '__main__':
    main()
