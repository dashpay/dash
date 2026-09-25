#!/usr/bin/env python3
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Check GitHub Actions workflows for security issues using zizmor.

Audit configuration lives in .github/zizmor.yml; individual findings are
ignored inline in the workflow with `# zizmor: ignore[<audit>] <reason>`.
"""

import shutil
import subprocess
import sys

CONFIG = '.github/zizmor.yml'
WORKFLOWS_DIR = '.github/workflows/'


def main():
    if shutil.which('zizmor') is None:
        print('Skipping GitHub Actions linting since zizmor is not installed.')
        sys.exit(0)

    zizmor_cmd = [
        'zizmor',
        '--offline',
        '--no-progress',
        '-qq',  # hide log lines (e.g. shell-type guesses); findings are still printed
        f'--config={CONFIG}',
        WORKFLOWS_DIR,
    ]
    if subprocess.run(zizmor_cmd).returncode != 0:
        print('zizmor reported GitHub Actions issues (see above). Fix them, or add an inline')
        print('`# zizmor: ignore[<audit>] <reason>` comment if a finding is a false positive.')
        sys.exit(1)


if __name__ == '__main__':
    main()
