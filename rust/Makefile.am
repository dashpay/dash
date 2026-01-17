# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

CARGO_BUILD_OPTS = --locked --target $(RUST_TARGET)

# 'cc' needs preserved case
RUST_TARGET_ENV_LOWER = $(shell echo '$(RUST_TARGET)' | tr '-' '_')
CARGO_ENV = \
  AR="$(AR)" \
  AR_$(RUST_TARGET_ENV_LOWER)="$(AR)" \
  CC="$(CC)" \
  CC_$(RUST_TARGET_ENV_LOWER)="$(CC)" \
  CFLAGS="$(CFLAGS)" \
  CFLAGS_$(RUST_TARGET_ENV_LOWER)="$(CFLAGS)" \
  CXX="$(CXX)" \
  CXX_$(RUST_TARGET_ENV_LOWER)="$(CXX)" \
  CXXFLAGS="$(CXXFLAGS)" \
  CXXFLAGS_$(RUST_TARGET_ENV_LOWER)="$(CXXFLAGS)" \
  MACOSX_DEPLOYMENT_TARGET="$(RUST_MACOS_DEPLOYMENT_TARGET)" \
  CARGO_INCREMENTAL="$(CARGO_INCREMENTAL)" \
  RUSTC="$(RUSTC)" \
  RUSTFLAGS="$(RUSTFLAGS)" \
  TERM="dumb" \
  ${RUST_OSX_SDK+SDKROOT="$(RUST_OSX_SDK)"}

if !ENABLE_DEBUG
CARGO_BUILD_OPTS += --release
endif # !ENABLE_DEBUG

if !SILENT_RULES
CARGO_BUILD_OPTS += --verbose
endif # !SILENT_RULES

include $(top_srcdir)/rust/Makefile.libs.include

all-local: cargo-build-chirp cxxbridge-chirp

cargo-build: cargo-build-chirp

cargo-clean: cargo-clean-chirp

clean-local: cargo-clean-chirp cxxbridge-clean-chirp clean-librustdeps-la

.PHONY: cargo-build cargo-clean
