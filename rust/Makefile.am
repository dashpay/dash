# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

CARGO_BUILD_OPTS = --locked --target $(RUST_TARGET)
CARGO_ENV = \
  MACOSX_DEPLOYMENT_TARGET="$(RUST_MACOS_DEPLOYMENT_TARGET)" \
  CARGO_INCREMENTAL="$(CARGO_INCREMENTAL)" \
  RUSTC="$(RUSTC)" \
  RUSTFLAGS="$(RUSTFLAGS)" \
  TERM="dumb" \
  ${RUST_OSX_SDK+SDKROOT="$(RUST_OSX_SDK)"}

# 'cargo' needs uppercase
RUST_NATIVE_ENV_UPPER = $(shell echo '$(RUST_NATIVE)' | tr 'a-z-' 'A-Z_')
CARGO_ENV += \
  AR="$(NATIVE_AR)" \
  CC="$(NATIVE_CC)" \
  CXX="$(NATIVE_CXX)" \
  CARGO_TARGET_$(RUST_NATIVE_ENV_UPPER)_LINKER="$(NATIVE_CC)"

# 'cc' needs preserved case
RUST_TARGET_ENV_LOWER = $(shell echo '$(RUST_TARGET)' | tr '-' '_')
# 'cargo' needs uppercase
RUST_TARGET_ENV_UPPER = $(shell echo '$(RUST_TARGET_ENV_LOWER)' | tr 'a-z' 'A-Z')
CARGO_ENV += \
  AR_$(RUST_TARGET_ENV_LOWER)="$(AR)" \
  CC_$(RUST_TARGET_ENV_LOWER)="$(CC)" \
  CFLAGS_$(RUST_TARGET_ENV_LOWER)="$(CFLAGS)" \
  CXX_$(RUST_TARGET_ENV_LOWER)="$(CXX)" \
  CXXFLAGS_$(RUST_TARGET_ENV_LOWER)="$(CXXFLAGS)" \
  CARGO_TARGET_$(RUST_TARGET_ENV_UPPER)_LINKER="$(CC)"

if !ENABLE_DEBUG
CARGO_BUILD_OPTS += --release
endif # !ENABLE_DEBUG

if !SILENT_RULES
CARGO_BUILD_OPTS += --verbose
endif # !SILENT_RULES

if ENABLE_ONLINE_RUST
CARGO_CONFIGURED = $(top_srcdir)/.cargo/.configured-for-online

$(CARGO_CONFIGURED):
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/.configured-for-offline
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/config.toml
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/config
	$(AM_V_at)touch $@
else
CARGO_BUILD_OPTS += --offline
CARGO_CONFIGURED = $(top_srcdir)/.cargo/.configured-for-offline

$(CARGO_CONFIGURED): $(top_srcdir)/.cargo/config.toml.offline
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/.configured-for-online
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/config
	$(AM_V_at)cp $< $(top_srcdir)/.cargo/config.toml
	$(AM_V_at)echo "directory = \"$(RUST_VENDORED_SOURCES)\"" >> $(top_srcdir)/.cargo/config.toml
	$(AM_V_at)touch $@
endif # ENABLE_ONLINE_RUST

include $(top_srcdir)/rust/Makefile.libs.include

all-local: $(CARGO_CONFIGURED) cargo-build-chirp cxxbridge-chirp

cargo-build: $(CARGO_CONFIGURED) cargo-build-chirp

cargo-clean: cargo-clean-chirp

cargo-clean-config:
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/.configured-for-online
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/.configured-for-offline
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/config.toml
	$(AM_V_at)rm -f $(top_srcdir)/.cargo/config

clean-local: cargo-clean-chirp cxxbridge-clean-chirp clean-librustdeps-la cargo-clean-config

.PHONY: cargo-build cargo-clean cargo-clean-config
