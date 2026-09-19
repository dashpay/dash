#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Build configuration for the dashbls package."""

import os
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext


class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir)


class CMakeBuild(build_ext):
    CMAKE_MINIMUM = (3, 18, 0)
    WINDOWS_GENERATOR_ARCH = (
        ("win-amd64", "x64"),
        ("win-arm64", "ARM64"),
        ("win32", "Win32"),
    )

    def _cmake(self) -> str:
        cmake = shutil.which("cmake")
        if cmake is None:
            raise RuntimeError("CMake must be installed to build dashbls")
        return cmake

    def run(self) -> None:
        cmake = self._cmake()
        try:
            out = subprocess.check_output(  # noqa: S603
                [cmake, "--version"], encoding="utf-8", errors="replace"
            )
        except (OSError, subprocess.CalledProcessError) as error:
            raise RuntimeError("cannot run cmake --version") from error
        found = re.search(r"version\s*([\d.]+)", out)
        if found is None:
            raise RuntimeError("cannot read a version out of: " + out.strip())
        parts = [int(part) for part in found.group(1).split(".")[:3] if part]
        while len(parts) < len(self.CMAKE_MINIMUM):
            parts.append(0)
        version = tuple(parts)
        if version < self.CMAKE_MINIMUM:
            raise RuntimeError(
                "CMake >= {} is required, found {}".format(
                    ".".join(str(p) for p in self.CMAKE_MINIMUM),
                    found.group(1),
                )
            )
        for ext in self.extensions:
            self.build_extension(ext)

    def build_extension(self, ext: CMakeExtension) -> None:
        extdir = os.path.abspath(os.path.dirname(self.get_ext_fullpath(ext.name)))
        cmake_args = [
            "-DBUILD_BLS_BENCHMARKS=OFF",
            "-DBUILD_BLS_TESTS=OFF",
            "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=" + extdir,
            "-DMULTI=",
            "-DPYBIND11_FINDPYTHON=ON",
            "-DPython_EXECUTABLE=" + sys.executable,
            "-DVERSION_INFO=" + self.distribution.get_version(),
        ]

        cmake_args += shlex.split(os.environ.get("CMAKE_ARGS", ""))

        try:
            import pybind11
        except ImportError:
            pass
        else:
            cmake_args.append("-Dpybind11_DIR=" + pybind11.get_cmake_dir())

        cfg = "Debug" if self.debug else "Release"
        build_args = ["--config", cfg, "--parallel", str(os.cpu_count() or 1)]

        if sys.platform == "win32":
            target = sysconfig.get_platform()
            arch = dict(self.WINDOWS_GENERATOR_ARCH).get(target)
            if arch is None:
                raise RuntimeError("unsupported windows platform: " + target)
            cmake_args.append(f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{cfg.upper()}={extdir}")
            generator = os.environ.get("CMAKE_GENERATOR", "Visual Studio")
            if generator.startswith("Visual Studio"):
                cmake_args += ["-A", arch]
                build_args += ["--", "/m"]
            else:
                cmake_args += ["-DCMAKE_BUILD_TYPE=" + cfg]
        else:
            cmake_args += ["-DCMAKE_BUILD_TYPE=" + cfg]

        os.makedirs(self.build_temp, exist_ok=True)
        cmake = self._cmake()
        subprocess.check_call(  # noqa: S603
            [cmake, ext.sourcedir, *cmake_args], cwd=self.build_temp
        )
        subprocess.check_call(  # noqa: S603
            [cmake, "--build", ".", *build_args], cwd=self.build_temp
        )


setup(
    package_dir={"": "binds/python"},
    packages=[],
    py_modules=[],
    ext_modules=[CMakeExtension("dashbls", os.path.dirname(os.path.abspath(__file__)))],
    cmdclass=dict(build_ext=CMakeBuild),
    zip_safe=False,
)
