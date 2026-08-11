# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

include $(top_srcdir)/rust/Makefile.common.include
include $(top_srcdir)/rust/Makefile.libs.include

EXTRA_DIST = \
	$(LIBRUSTDEPS_FILES) \
	$(LIBRUST_CHIRP_FILES)

if ENABLE_RUST
all-local: $(CARGO_CONFIGURED) cargo-build-dashrust cxxbridge-chirp

cargo-build: $(CARGO_CONFIGURED) cargo-build-dashrust
else
all-local:

cargo-build:
endif

cargo-clean: cargo-clean-dashrust

# Aggregate dist hook: per-crate includes define dist-<crate> targets; new
# crates append here instead of defining their own dist-hook (automake allows
# only one dist-hook recipe per Makefile).
if ENABLE_RUST
dist-hook: dist-chirp
endif

cargo-clean-config:
	$(AM_V_at)rm -f $(abs_top_builddir)/.cargo/.configured-for-online
	$(AM_V_at)rm -f $(abs_top_builddir)/.cargo/.configured-for-offline
	$(AM_V_at)rm -f $(CARGO_BUILD_CONFIG)
	$(AM_V_at)rm -f $(RUST_NATIVE_LINKER) $(RUST_TARGET_LINKER)

clean-local: cargo-clean-dashrust cxxbridge-clean-chirp cargo-clean-config

.PHONY: cargo-build cargo-clean cargo-clean-config
