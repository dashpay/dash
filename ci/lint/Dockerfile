# See test/lint/README.md for usage.
#
# This container basically has to live in this directory in order to pull in the CI
# install scripts. If it lived in the root directory, it would have to pull in the
# entire repo as docker context during build; if it lived elsewhere, it wouldn't be
# able to make back-references to pull in the install scripts. So here it lives.

# Builder for cppcheck; keep the version in sync with
# contrib/containers/ci/ci-slim.Dockerfile so results match the CI lint job.
FROM debian:bookworm-slim AS cppcheck-builder
ARG CPPCHECK_VERSION=2.21.0
ARG CPPCHECK_ARCHIVE_SHA256=f028ff75ca5372738f3737c8b3e8611426a6526b6aea2ef01301ab0f5902f044
RUN set -ex; \
    apt-get update && apt-get install -y --no-install-recommends \
        curl \
        ca-certificates \
        cmake \
        make \
        g++ \
    && rm -rf /var/lib/apt/lists/*; \
    echo "Downloading Cppcheck version: ${CPPCHECK_VERSION}"; \
    curl -fL "https://github.com/danmar/cppcheck/archive/${CPPCHECK_VERSION}.tar.gz" -o /tmp/cppcheck.tar.gz; \
    echo "${CPPCHECK_ARCHIVE_SHA256}  /tmp/cppcheck.tar.gz" | sha256sum -c -; \
    mkdir -p /src/cppcheck && tar -xzf /tmp/cppcheck.tar.gz -C /src/cppcheck --strip-components=1; \
    rm /tmp/cppcheck.tar.gz; \
    cd /src/cppcheck; \
    mkdir build && cd build && cmake .. && cmake --build . -j"$(nproc)"; \
    strip bin/cppcheck

FROM python:3.10.19-bookworm
ARG TARGETARCH

ENV DEBIAN_FRONTEND=noninteractive
ENV LC_ALL=C.UTF-8

COPY --from=cppcheck-builder /src/cppcheck/build/bin/cppcheck /usr/local/bin/cppcheck
COPY --from=cppcheck-builder /src/cppcheck/cfg /usr/local/share/Cppcheck/cfg

# This is used by the 04_install.sh script; we can't read the Python version from
# .python-version for the same reasons as above, and it's more efficient to pull a
# preexisting Python image than it is to build from source.
ENV SKIP_PYTHON_INSTALL=1

# Must be built from ./ci/lint/ for these paths to work.
COPY ./docker-entrypoint.sh /entrypoint.sh
COPY ./04_install.sh /install.sh

RUN /install.sh && \
  echo 'alias lint="./ci/lint/06_script.sh"' >> ~/.bashrc && \
  chmod 755 /entrypoint.sh && \
  rm -rf /var/lib/apt/lists/*


WORKDIR /bitcoin
ENTRYPOINT ["/entrypoint.sh"]
