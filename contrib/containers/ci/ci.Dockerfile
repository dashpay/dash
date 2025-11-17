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
    bsdmainutils \
    ccache \
    cmake \
    g++-arm-linux-gnueabihf \
    g++-mingw-w64-x86-64 \
    gawk \
    gettext \
    libtool \
    m4 \
    parallel \
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

# Install Nix to fetch versioned GCC releases
RUN set -ex; \
    curl -fsSL https://install.determinate.systems/nix | sh -s -- install linux \
    --determinate \
    --extra-conf "sandbox = false" \
    --no-confirm \
    --init none
ENV PATH="/nix/var/nix/profiles/default/bin:${PATH}" \
    NIX_CONF_DIR="/etc/nix"
COPY --from=docker_root ./nix.custom.conf /etc/nix/nix.custom.conf
COPY --from=docker_root ./default.nix /etc/nix/
RUN set -ex; \
    nix-env -f /etc/nix/default.nix -i; \
    nix-collect-garbage -d; \
    nix-store --optimise;
RUN set -ex; \
    chmod -R o+rX /nix/store /nix/var/nix/profiles/default; \
    mkdir -p /usr/local/bin /usr/local/lib /usr/local/lib64; \
    for bin in /nix/var/nix/profiles/default/bin/*; do \
        if [ -f "$bin" ]; then \
            ln -sf "$bin" "/usr/local/bin/$(basename "$bin")"; \
        fi; \
    done; \
    for dir in /nix/var/nix/profiles/default/lib/*; do \
        if [ -d "$dir" ]; then \
            ln -sf "$dir" "/usr/local/lib/$(basename "$dir")"; \
        fi; \
    done; \
    for dir in /nix/var/nix/profiles/default/lib64/*; do \
        if [ -d "$dir" ]; then \
            ln -sf "$dir" "/usr/local/lib64/$(basename "$dir")"; \
        fi; \
    done; \
    ln -sf "/usr/lib/$(dpkg-architecture -qDEB_HOST_MULTIARCH)" "/usr/lib/multiarch"
# Nix-supplied compilers won't search for distro-installed libraries without explicit
# specification and we can't run binaries built with those compilers without specifying
# where their runtime dependencies are
ENV LD_LIBRARY_PATH="/usr/local/lib64/gcc-15:/usr/local/lib/gcc-15:/usr/local/lib64/gcc-11:/usr/local/lib/gcc-11:${LD_LIBRARY_PATH}"
ENV LIBRARY_PATH="${LD_LIBRARY_PATH}:/usr/lib/multiarch:/usr/lib"

RUN \
  mkdir -p /cache/ccache && \
  mkdir /cache/depends && \
  mkdir /cache/sdk-sources && \
  chown ${USER_ID}:${GROUP_ID} /cache && \
  chown ${USER_ID}:${GROUP_ID} -R /cache

# We're done, switch back to non-privileged user
USER dash
