FROM ubuntu:jammy

# Needed to prevent tzdata hanging while expecting user input
ENV DEBIAN_FRONTEND="noninteractive" TZ="Europe/London"

# Build and base stuff
# (zlib1g-dev is needed for the Qt host binary builds, but should not be used by target binaries)
ENV APT_ARGS="-y --no-install-recommends --no-upgrade"

# Install packages for i386; enabled on amd64 hosts
SHELL ["/bin/bash", "-c"]
RUN set -ex; \
    if [[ "$(dpkg --print-architecture)" == "amd64" ]]; then \
      dpkg --add-architecture i386 && \
      apt-get update && apt-get install $APT_ARGS \
      g++-multilib \
      linux-libc-dev:i386 \
      wine32 \
      && rm -rf /var/lib/apt/lists/*; \
    fi;

RUN apt-get update && apt-get install $APT_ARGS \
    autotools-dev \
    automake \
    autoconf \
    bison \
    build-essential \
    bsdmainutils \
    ca-certificates \
    ccache \
    cmake \
    curl \
    g++ \
    gettext \
    git \
    libc++-dev \
    libc++abi-dev \
    libtool \
    libxcb-icccm4 \
    libxcb-image0 \
    libxcb-keysyms1 \
    libxcb-randr0 \
    libxcb-render-util0 \
    libxcb-shape0 \
    libxcb-sync1 \
    libxcb-xfixes0 \
    libxcb-xinerama0 \
    libxcb-xkb1 \
    libxkbcommon-x11-0 \
    linux-libc-dev \
    wget \
    unzip \
    m4 \
    pkg-config \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Compile and install GCC
ARG GCC_VERSION=14.2.0
ARG GCC_COMPILE_ARGS=""
RUN set -ex; \
    curl -fL https://ftpmirror.gnu.org/gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz -o /tmp/gcc.tar.xz; \
    mkdir -p /tmp/gcc; \
    tar -xf /tmp/gcc.tar.xz -C /tmp/gcc --strip-components=1; \
    rm /tmp/gcc.tar.xz; \
    cd /tmp/gcc; \
    ./contrib/download_prerequisites; \
    (rm *.tar.* || true); \
    dpkgArch="$(dpkg --print-architecture)"; GCC_PLATFORM_ARGS=""; \
    case "${dpkgArch##*-}" in  \
      armel) GCC_PLATFORM_ARGS="--with-arch=armv4t --with-float=soft";; \
      armhf) GCC_PLATFORM_ARGS="--with-arch=armv7-a --with-float=hard --with-fpu=vfpv3-d16 --with-mode=thumb";; \
      i386|i686) GCC_PLATFORM_ARGS="--with-arch=i686";; \
      amd64) GCC_PLATFORM_ARGS="--enable-multilib --with-abi=m64 --with-arch-32=i686 --with-multilib-list=m32,m64,mx32 --without-isl";; \
    esac; \
    gnuArch="$(dpkg-architecture --query DEB_BUILD_GNU_TYPE)"; \
    ln -s "/usr/include/${gnuArch}/asm" "/usr/include/asm"; \
    majorVersion="${GCC_VERSION%%.*}"; \
    ./configure \
      --build="${gnuArch}" \
      --host="${gnuArch}" \
      --target="${gnuArch}" \
      --disable-bootstrap \
      --disable-vtable-verify \
      --disable-werror \
      --enable-checking=release \
      --enable-clocale=gnu \
      --enable-default-pie \
      --enable-gnu-unique-object \
      --enable-languages=c,c++,fortran,objc,obj-c++ \
      --enable-libstdcxx-debug \
      --enable-libstdcxx-time=yes \
      --enable-libsanitizer \
      --enable-linker-build-id \
      --enable-nls \
      --enable-threads=posix \
      --prefix="/opt/gcc" \
      --program-suffix="-${majorVersion}" \
      --with-default-libstdcxx-abi=new \
      --with-gcc-major-version-only \
      --with-tune=generic \
      ${GCC_COMPILE_ARGS} \
      ${GCC_PLATFORM_ARGS}; \
    make -j "$(( $(nproc) - 1 ))"; \
    make install-strip; \
    cd /tmp; \
    rm -rf /tmp/gcc;
ENV LD_LIBRARY_PATH="/opt/gcc/lib:/opt/gcc/lib64:${LD_LIBRARY_PATH}"
ENV PATH="/opt/gcc/bin:${PATH}"

# Install Clang+LLVM
ARG LLVM_VERSION=18
RUN apt-get update && apt-get install $APT_ARGS \
    lsb-release \
    software-properties-common \
    gnupg \
    && rm -rf /var/lib/apt/lists/*

RUN cd /tmp && \
    wget https://apt.llvm.org/llvm.sh && \
    chmod +x llvm.sh && \
    /tmp/llvm.sh ${LLVM_VERSION} && \
    rm -rf /tmp/llvm.sh

# Python setup
# PYTHON_VERSION should match the value in .python-version
ARG PYTHON_VERSION=3.9.18
RUN apt-get update && apt-get install $APT_ARGS \
    libbz2-dev \
    libffi-dev \
    liblzma-dev \
    libncurses5-dev \
    libncursesw5-dev \
    libreadline-dev \
    libsqlite3-dev \
    libssl-dev \
    make \
    tk-dev \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

ENV PYENV_ROOT="/usr/local/pyenv"
ENV PATH="${PYENV_ROOT}/shims:${PYENV_ROOT}/bin:${PATH}"
RUN curl https://pyenv.run | bash \
    && pyenv update \
    && pyenv install $PYTHON_VERSION \
    && pyenv global $PYTHON_VERSION \
    && pyenv rehash
RUN pip3 install \
    codespell==1.17.1 \
    flake8==3.8.3 \
    jinja2 \
    lief==0.13.2 \
    pyzmq \
    vulture==2.3 \
    mypy==0.781 \
    yq \
    multiprocess

ARG DASH_HASH_VERSION=1.4.0
RUN set -ex; \
    cd /tmp; \
    git clone --depth 1 --no-tags --branch=${DASH_HASH_VERSION} https://github.com/dashpay/dash_hash; \
    cd dash_hash && pip3 install -r requirements.txt .; \
    cd .. && rm -rf dash_hash

ARG CPPCHECK_VERSION=2.13.0
RUN set -ex; \
    curl -fL "https://github.com/danmar/cppcheck/archive/${CPPCHECK_VERSION}.tar.gz" -o /tmp/cppcheck.tar.gz; \
    mkdir -p /opt/cppcheck && tar -xzf /tmp/cppcheck.tar.gz -C /opt/cppcheck --strip-components=1 && rm /tmp/cppcheck.tar.gz; \
    cd /opt/cppcheck; \
    mkdir build && cd build && cmake .. && cmake --build . -j "$(( $(nproc) - 1 ))"; \
    mkdir /usr/local/share/Cppcheck && ln -s /opt/cppcheck/cfg/ /usr/local/share/Cppcheck/cfg; \
    rm -rf /tmp/cppcheck.tar.gz
ENV PATH="/opt/cppcheck/build/bin:${PATH}"

ARG SHELLCHECK_VERSION=v0.7.1
RUN set -ex; \
    curl -fL "https://github.com/koalaman/shellcheck/releases/download/${SHELLCHECK_VERSION}/shellcheck-${SHELLCHECK_VERSION}.linux.x86_64.tar.xz" -o /tmp/shellcheck.tar.xz; \
    mkdir -p /opt/shellcheck && tar -xf /tmp/shellcheck.tar.xz -C /opt/shellcheck --strip-components=1 && rm /tmp/shellcheck.tar.xz
ENV PATH="/opt/shellcheck:${PATH}"

# Add user with specified (or default) user/group ids
# and setup configuration files
ARG USER_ID=1000
ARG GROUP_ID=1000
RUN set -ex; \
    groupadd -g ${GROUP_ID} dash; \
    useradd -u ${USER_ID} -g dash -s /bin/bash -m -d /home/dash dash; \
    mkdir -p /home/dash/.config/gdb; \
    echo "add-auto-load-safe-path /opt/gcc/lib:/opt/gcc/lib64" | tee /home/dash/.config/gdb/gdbinit; \
    chown ${USER_ID}:${GROUP_ID} -R /home/dash

# Packages needed for all target builds
RUN apt-get update && apt-get install $APT_ARGS \
    bc \
    gawk \
    g++-arm-linux-gnueabihf \
    g++-mingw-w64-x86-64 \
    jq \
    libncurses5 \
    nsis \
    python3-zmq \
    parallel \
    valgrind \
    wine-stable \
    wine64 \
    zip \
    && rm -rf /var/lib/apt/lists/*

# This is a hack. It is needed because gcc-multilib and g++-multilib are conflicting with g++-arm-linux-gnueabihf. This is
# due to gcc-multilib installing the following symbolic link, which is needed for -m32 support. However, this causes
# arm builds to also have the asm folder implicitly in the include search path. This is kind of ok, because the asm folder
# for arm has precedence.
RUN ln -s "/usr/include/$(dpkg-architecture --query DEB_BUILD_GNU_TYPE)/asm" "/usr/include/asm"

# Make sure std::thread and friends is available
RUN \
  update-alternatives --set x86_64-w64-mingw32-gcc  /usr/bin/x86_64-w64-mingw32-gcc-posix; \
  update-alternatives --set x86_64-w64-mingw32-g++  /usr/bin/x86_64-w64-mingw32-g++-posix; \
  exit 0

RUN \
  mkdir -p /src/dash && \
  mkdir -p /cache/ccache && \
  mkdir /cache/depends && \
  mkdir /cache/sdk-sources && \
  chown ${USER_ID}:${GROUP_ID} /src && \
  chown ${USER_ID}:${GROUP_ID} -R /src && \
  chown ${USER_ID}:${GROUP_ID} /cache && \
  chown ${USER_ID}:${GROUP_ID} -R /cache

WORKDIR /src/dash

USER dash
