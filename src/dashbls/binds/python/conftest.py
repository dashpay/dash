#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Allow samples to be imported by name."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent / "samples"))
