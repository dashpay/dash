# syntax = devthefuture/dockerfile-x

FROM ./ci.Dockerfile

# MemorySanitizer reports any read of memory it did not watch being written,
# so the C++ standard library has to be instrumented too. A stock libstdc++ or
# libc++ produces a flood of reports that cannot be told apart from real ones.
#
# Upstream builds this in ci/test/01_base_install.sh, which runs inside its
# docker build. Dash has no such script, so it lives here instead, which keeps
# the result in a cached image layer rather than rebuilding it on every run.

USER root

ARG LLVM_VERSION=19
ARG LLVM_TAG=llvmorg-19.1.7

RUN set -ex; \
    apt-get update && apt-get install ${APT_ARGS} ninja-build; \
    rm -rf /var/lib/apt/lists/*; \
    git clone --depth=1 -b "${LLVM_TAG}" https://github.com/llvm/llvm-project /llvm-project; \
    cmake -G Ninja -B /cxx_build/ \
      -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
      -DCMAKE_BUILD_TYPE=Release \
      -DLLVM_USE_SANITIZER=MemoryWithOrigins \
      -DCMAKE_C_COMPILER=clang-${LLVM_VERSION} \
      -DCMAKE_CXX_COMPILER=clang++-${LLVM_VERSION} \
      -DLLVM_TARGETS_TO_BUILD=Native \
      -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
      -DLIBCXX_HARDENING_MODE=debug \
      -S /llvm-project/runtimes; \
    ninja -C /cxx_build/; \
    du -sh /llvm-project; \
    rm -rf /llvm-project;

USER dash
