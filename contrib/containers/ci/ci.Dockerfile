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

# ccache from upstream rather than apt: remote storage over HTTPS needs a
# storage helper, which ccache supports from 4.13 (noble ships 4.9.1).
ARG TARGETARCH
ARG CCACHE_VERSION=4.14.1
ARG CCACHE_HTTP_HELPER_VERSION=0.10
RUN set -ex; \
    ARCH_INFERRED="${TARGETARCH}"; \
    if [ -z "${ARCH_INFERRED}" ]; then \
        ARCH_INFERRED="$(dpkg --print-architecture || true)"; \
    fi; \
    case "${ARCH_INFERRED}" in \
        amd64|x86_64) \
            CCACHE_ARCH="x86_64"; HELPER_ARCH="amd64"; \
            CCACHE_SHA256="ad63d19f5d09ea13f749651653a561c98a59994e673504a4266617b8754218f7"; \
            HELPER_SHA256="88963ef2cc21ca588145d46bcb02b8295275975ed75696eedf2cdb0e9543edd0" ;; \
        arm64|aarch64) \
            CCACHE_ARCH="aarch64"; HELPER_ARCH="arm64"; \
            CCACHE_SHA256="4e8f16aa3b55acc57dc24ab3b997bf2bcef049a3bde1c97e38b07397e4ed4f6d"; \
            HELPER_SHA256="f0ee3302b52f87628b69fa53d2cc52a626c4c5c7e43b779ac9e3db481418db40" ;; \
        *) echo "Unsupported architecture for ccache: ${ARCH_INFERRED}"; exit 1 ;; \
    esac; \
    curl -fL "https://github.com/ccache/ccache/releases/download/v${CCACHE_VERSION}/ccache-${CCACHE_VERSION}-linux-${CCACHE_ARCH}-glibc.tar.xz" -o /tmp/ccache.tar.xz; \
    echo "${CCACHE_SHA256}  /tmp/ccache.tar.xz" | sha256sum -c -; \
    tar -xf /tmp/ccache.tar.xz -C /tmp --strip-components=1 "ccache-${CCACHE_VERSION}-linux-${CCACHE_ARCH}-glibc/ccache"; \
    install -m 0755 /tmp/ccache /usr/local/bin/ccache; \
    curl -fL "https://github.com/ccache/ccache-storage-http-go/releases/download/v${CCACHE_HTTP_HELPER_VERSION}/ccache-storage-http-go-${CCACHE_HTTP_HELPER_VERSION}-linux-${HELPER_ARCH}.tar.gz" -o /tmp/helper.tar.gz; \
    echo "${HELPER_SHA256}  /tmp/helper.tar.gz" | sha256sum -c -; \
    tar -xf /tmp/helper.tar.gz -C /tmp --strip-components=1 "ccache-storage-http-go-${CCACHE_HTTP_HELPER_VERSION}-linux-${HELPER_ARCH}/ccache-storage-http"; \
    install -m 0755 /tmp/ccache-storage-http /usr/local/bin/ccache-storage-https; \
    rm -f /tmp/ccache.tar.xz /tmp/ccache /tmp/helper.tar.gz /tmp/ccache-storage-http; \
    ccache --version | head -n 1

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

RUN \
  mkdir -p /cache/ccache && \
  mkdir /cache/ctcache && \
  mkdir /cache/depends && \
  mkdir /cache/sdk-sources && \
  chown ${USER_ID}:${GROUP_ID} /cache && \
  chown ${USER_ID}:${GROUP_ID} -R /cache

# We're done, switch back to non-privileged user
USER dash
