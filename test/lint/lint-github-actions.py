#!/usr/bin/env python3
#
# Copyright (c) 2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Check for security issues in GitHub Actions workflow files using zizmor.
"""

import subprocess
import sys
import tempfile
import os

# Disabled audits:
# These are intentionally disabled and don't indicate security issues in our context.
DISABLED = [
    'unpinned-uses',   # We use version tags rather than SHA pinning
    'unpinned-images', # Container images use dynamic tags
]

# Ignored findings for specific files/locations:
# Format: 'audit-name': ['filename.yml:line', ...]
# Note: Use base filename only, not full path
IGNORED = {
    # pull_request_target is used intentionally in these workflows with proper
    # safeguards (explicit checkout of PR head SHA, limited permissions)
    'dangerous-triggers': [
        'build.yml:3',
        'guix-build.yml:3',
        'label-merge-conflicts.yml:2',
        'merge-check.yml:6',
        'predict-conflicts.yml:3',
        'semantic-pull-request.yml:3',
    ],
    # inputs.context is passed to docker/build-push-action but only from internal
    # workflow_call callers with hardcoded paths - not user-controllable
    'template-injection': [
        'build-container.yml:60',
    ],
    # packages:write at workflow level is required because reusable workflows
    # (build-container.yml) inherit caller permissions and need it to push to ghcr.io
    'excessive-permissions': [
        'build.yml:9',
    ],
}


def check_zizmor_install():
    try:
        subprocess.run(
            ['zizmor', '--version'],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True
        )
    except FileNotFoundError:
        print('Skipping GitHub Actions linting since zizmor is not installed.')
        print('Install with: pip install zizmor')
        sys.exit(0)


def generate_config():
    """Generate zizmor configuration with disabled and ignored rules."""
    lines = ['rules:']

    # Add disabled audits
    for audit in DISABLED:
        lines.append(f'  {audit}:')
        lines.append(f'    disable: true')

    # Add ignored findings
    for audit, locations in IGNORED.items():
        lines.append(f'  {audit}:')
        lines.append(f'    ignore:')
        for loc in locations:
            lines.append(f'      - {loc}')

    return '\n'.join(lines) + '\n'


def main():
    check_zizmor_install()

    # Create a temporary config file
    config_content = generate_config()

    with tempfile.NamedTemporaryFile(mode='w', suffix='.yml', delete=False) as f:
        f.write(config_content)
        config_path = f.name

    try:
        # Build the zizmor command
        zizmor_cmd = [
            'zizmor',
            '--config', config_path,
            '.github/workflows/',
        ]

        # Run zizmor
        result = subprocess.run(zizmor_cmd)

        # zizmor returns non-zero if it finds issues
        if result.returncode != 0:
            print('GitHub Actions security issues found. Please fix the above issues.')
            sys.exit(1)
    finally:
        # Clean up temp file
        os.unlink(config_path)


if __name__ == '__main__':
    main()
