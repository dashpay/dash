# syntax = devthefuture/dockerfile-x

FROM ./ci-slim.Dockerfile

# The inherited Dockerfile switches to non-privileged context and we've
# just started configuring this image, give us root access
USER root

# Install packages
RUN set -ex; \
    apt-get update && apt-get install ${APT_ARGS} \
    autoconf \
    automake \
    autotools-dev \
    bc \
    bear \
    bison \
    ccache \
    cmake \
    g++-11 \
    g++-14 \
    g++-aarch64-linux-gnu \
    g++-mingw-w64-x86-64 \
    g++-x86-64-linux-gnu \
    gawk \
    gettext \
    libtool \
    m4 \
    pkg-config \
    wine-stable \
    wine64 \
    zip \
    && rm -rf /var/lib/apt/lists/*

# Install Clang + LLVM and set it as default
RUN set -ex; \
    apt-get update && apt-get install ${APT_ARGS} \
    "clang-${LLVM_VERSION}" \
    "clangd-${LLVM_VERSION}" \
    "clang-format-${LLVM_VERSION}" \
    "clang-tidy-${LLVM_VERSION}" \
    "libc++-${LLVM_VERSION}-dev" \
    "libc++abi-${LLVM_VERSION}-dev" \
    "libclang-${LLVM_VERSION}-dev" \
    "libclang-rt-${LLVM_VERSION}-dev" \
    "lld-${LLVM_VERSION}" \
    "lldb-${LLVM_VERSION}"; \
    rm -rf /var/lib/apt/lists/*; \
    echo "Setting defaults..."; \
    llvmUpdAltArgs="update-alternatives --install /usr/bin/llvm-config llvm-config /usr/bin/llvm-config-${LLVM_VERSION} 100"; \
    for binName in clang clang++ clang-apply-replacements clang-format clang-tidy clangd dsymutil lld lldb lldb-server llvm-ar llvm-cov llvm-nm llvm-objdump llvm-ranlib llvm-strip run-clang-tidy; do \
        llvmUpdAltArgs="${llvmUpdAltArgs} --slave /usr/bin/${binName} ${binName} /usr/bin/${binName}-${LLVM_VERSION}"; \
    done; \
    for binName in ld64.lld ld.lld lld-link wasm-ld; do \
        llvmUpdAltArgs="${llvmUpdAltArgs} --slave /usr/bin/${binName} ${binName} /usr/bin/lld-${LLVM_VERSION}"; \
    done; \
    sh -c "${llvmUpdAltArgs}";
# LD_LIBRARY_PATH is empty by default, this is the first entry
ENV LD_LIBRARY_PATH="/usr/lib/llvm-${LLVM_VERSION}/lib"

RUN set -ex; \
    git clone --depth=1 "https://github.com/include-what-you-use/include-what-you-use" -b "clang_${LLVM_VERSION}" /opt/iwyu; \
    cd /opt/iwyu; \
    mkdir build && cd build; \
    cmake -G 'Unix Makefiles' -DCMAKE_PREFIX_PATH=/usr/lib/llvm-${LLVM_VERSION} ..; \
    make install -j "$(( $(nproc) - 1 ))"; \
    cd /opt && rm -rf /opt/iwyu;

# Install ctcache for clang-tidy result caching
# Pin to specific commit to ensure patch applies correctly
ARG CTCACHE_COMMIT=e393144d5c49b060a1dbc7ae15b9c6973efb967d
RUN set -ex; \
    mkdir -p /usr/local/bin/src/ctcache; \
    curl -fsSL "https://raw.githubusercontent.com/matus-chochlik/ctcache/${CTCACHE_COMMIT}/src/ctcache/clang_tidy_cache.py" \
        -o /usr/local/bin/src/ctcache/clang_tidy_cache.py; \
    curl -fsSL "https://raw.githubusercontent.com/matus-chochlik/ctcache/${CTCACHE_COMMIT}/clang-tidy" \
        -o /usr/local/bin/clang-tidy-cache; \
    chmod +x /usr/local/bin/clang-tidy-cache;

# The SDK lives in the image rather than in a per-run cache. depends/Makefile
# looks for it under depends/SDKs, inside the checkout, so point it here.
# Keep the version in sync with depends/hosts/darwin.mk.
ARG XCODE_VERSION=26.1.1
ARG XCODE_BUILD_ID=17B100
ARG OSX_SDK_SHA256=9600fa93644df674ee916b5e2c8a6ba8dacf631996a65dc922d003b98b5ea3b1
ENV SDK_PATH=/opt/SDKs
RUN set -ex; \
    OSX_SDK_BASENAME="Xcode-${XCODE_VERSION}-${XCODE_BUILD_ID}-extracted-SDK-with-libcxx-headers"; \
    curl -fL "https://bitcoincore.org/depends-sources/sdks/${OSX_SDK_BASENAME}.tar" -o /tmp/sdk.tar; \
    echo "${OSX_SDK_SHA256}  /tmp/sdk.tar" | sha256sum -c -; \
    mkdir -p "${SDK_PATH}"; \
    tar -C "${SDK_PATH}" -xf /tmp/sdk.tar; \
    rm -f /tmp/sdk.tar

RUN \
  mkdir -p /cache/ccache && \
  mkdir /cache/ctcache && \
  mkdir /cache/depends && \
  mkdir /cache/sdk-sources && \
  chown ${USER_ID}:${GROUP_ID} /cache && \
  chown ${USER_ID}:${GROUP_ID} -R /cache

# We're done, switch back to non-privileged user
USER dash
