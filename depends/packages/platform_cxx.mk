# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# dash-sdk-cxx, the C++ binding of the Dash Platform SDK (packages/rs-sdk-cxx
# of dashpay/platform), built from two sha256-pinned archives: the Platform
# source tarball at the pinned commit, and the crate bundle that
# contrib/devtools/platform-bundle.sh produces for that commit (trimmed
# workspace, pruned Cargo.lock, vendored crates, the Tenderdash sources
# tenderdash-proto generates its protobuf code from, Cargo configuration).
# Cargo runs with --frozen and never touches the network.
#
# Both archives must be on the depends sources mirror (FALLBACK_DOWNLOAD_PATH)
# before this is merged. The bundle has no upstream URL, and GitHub does not
# promise stable bytes for the archive endpoint the tarball comes from.
# Whoever bumps the pin runs platform-bundle.sh, which prints the lines to
# update here, and uploads both. Until then, a bundle the script wrote to
# SOURCES_PATH is used as is; its hash is checked on extraction either way.

package=platform_cxx
$(package)_version=dc4dc8e80ef8c495be693d4cc19c378a94f5d116
$(package)_download_path=https://github.com/dashpay/platform/archive
$(package)_download_file=$($(package)_version).tar.gz
$(package)_file_name=platform-$($(package)_version).tar.gz
$(package)_sha256_hash=fca5a310c8322924734ae7f8c522bffe4622bb4eb8394a6e054f56129ffbe592
$(package)_crates_file_name=dash-sdk-cxx-crates-$($(package)_version).tar.gz
$(package)_crates_sha256_hash=6f9d50e3c82406956b4b769874824bac7c28e8f1bcf1b3d514f7d381ea672f55
$(package)_fetched_id=$($(package)_crates_sha256_hash)
$(package)_extra_sources=$($(package)_crates_file_name)
$(package)_dependencies=native_rust rust_stdlib native_protobuf
$(package)_patches=rustc-linker.sh build-linker.sh check-native-libs.sh

# The system libraries the archive needs, which dash_sdk_cxx.pc passes on
# to configure. Each is vetted against contrib/guix/symbol-check.py, and the
# build fails if rustc reports one (--print=native-static-libs) that is
# neither here nor in the ignored list below.
$(package)_native_libs_linux=-lpthread -ldl -lm
$(package)_native_libs_darwin=-framework Security -framework CoreFoundation
$(package)_native_libs_mingw32=-lbcrypt -ladvapi32 -lkernel32 -lntdll -luserenv -lws2_32 -ldbghelp

# Libraries rustc reports that are deliberately not passed on:
# - the C++ runtime the cxx bridge needs, which the C++ link of dash-qt
#   already has: -lstdc++, and -lc++ on macOS;
# - the toolchain's own C runtime, which every link already has: -lc,
#   -lgcc_s, -lgcc_eh and -lgcc, and on mingw-w64 -lmsvcrt, -lmingwex,
#   -lmingw32 and -l:libpthread.a;
# - glibc's -lutil and -lrt: symbol-check.py does not allow them, and the
#   glibc standard library imports nothing from them (every symbol it uses
#   from glibc 2.31 is in libc, libpthread, libdl or libm);
# - macOS's -liconv, -lSystem, -lc and -lm, which the macOS linker adds anyway.
$(package)_native_libs_ignored_linux=-lstdc++ -lc -lgcc_s -lgcc_eh -lgcc -lutil -lrt
$(package)_native_libs_ignored_darwin=-lc++ -liconv -lSystem -lc -lm
$(package)_native_libs_ignored_mingw32=-lstdc++ -lmsvcrt -lmingwex -lmingw32 -lgcc_eh -lgcc -l:libpthread.a

define $(package)_fetch_cmds
$(call fetch_file,$(package),$($(package)_download_path),$($(package)_download_file),$($(package)_file_name),$($(package)_sha256_hash)) && \
( echo "$($(package)_crates_sha256_hash)  $($(package)_source_dir)/$($(package)_crates_file_name)" | $(build_SHA256SUM) -c >/dev/null 2>&1 || \
  ( rm -f $($(package)_source_dir)/$($(package)_crates_file_name) && \
    $(call fetch_file_inner,$(package),$(FALLBACK_DOWNLOAD_PATH),$($(package)_crates_file_name),$($(package)_crates_file_name),$($(package)_crates_sha256_hash)) ) )
endef

# Only packages/ is needed from the Platform tarball; the bundle supplies the
# workspace manifest, lock and Cargo configuration.
define $(package)_extract_cmds
  echo "$($(package)_sha256_hash)  $($(package)_source)" > .$($(package)_file_name).hash && \
  echo "$($(package)_crates_sha256_hash)  $($(package)_source_dir)/$($(package)_crates_file_name)" >> .$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c .$($(package)_file_name).hash && \
  $(build_TAR) --no-same-owner --strip-components=1 -xf $($(package)_source) platform-$($(package)_version)/packages && \
  $(build_TAR) --no-same-owner -xf $($(package)_source_dir)/$($(package)_crates_file_name)
endef

$(package)_rust_target=$(rust_stdlib_target)
$(package)_cc_target=$(subst -,_,$($(package)_rust_target))
$(package)_build_linker_var:=CARGO_TARGET_$(shell echo $(native_rust_build_target) | tr a-z- A-Z_)_LINKER

# Only the bundle's .cargo/config.toml configures Cargo:
# - Cargo reads .cargo/config.toml in the directory it runs in and in every
#   parent, whatever --manifest-path says, so it runs from / and is given the
#   bundle's file with --config. Configuration above the depends tree, such as
#   ~/.cargo/config.toml when the tree is under a home directory, is therefore
#   never read, and the build works wherever the tree is; preprocess fails if
#   / itself has one.
# - The Cargo home is private and empty.
# - Environment variables that would change the build are removed:
#   RUSTC_WORKSPACE_WRAPPER, RUSTC_BOOTSTRAP, CARGO_ENCODED_RUSTFLAGS,
#   __CARGO_DEFAULT_LIB_METADATA, and all CARGO_BUILD_*, CARGO_PROFILE_*,
#   CARGO_TARGET_* and CARGO_UNSTABLE_* ones, and the <VAR>_<triple> forms
#   (such as CC_x86_64-unknown-linux-gnu) that cc-rs prefers over the
#   <VAR>_<triple_with_underscores> ones set below; TENDERDASH_DIR and
#   TENDERDASH_COMMITISH, which the bundle's configuration sets to the tag of
#   the Tenderdash archive it carries.
# - The release profile is pinned to Platform's, which is Cargo's default:
#   opt-level 3, no debug info, no LTO, 16 codegen units, no incremental
#   compilation, and unwinding panics (the crate refuses panic=abort). Nothing
#   Cargo links here is installed (build scripts, the cdylib dash-sdk also
#   declares), so nothing is stripped; on macOS that would need rust-objcopy
#   and an LLVM library native_rust does not stage.
$(package)_unset_env:=RUSTC_WORKSPACE_WRAPPER RUSTC_BOOTSTRAP CARGO_ENCODED_RUSTFLAGS __CARGO_DEFAULT_LIB_METADATA
$(package)_unset_env+=TENDERDASH_DIR TENDERDASH_COMMITISH
$(package)_unset_env+=$(filter CARGO_BUILD_% CARGO_PROFILE_% CARGO_TARGET_% CARGO_UNSTABLE_%,$(.VARIABLES))
$(package)_unset_env+=$(filter %_$(rust_stdlib_target) %_$(native_rust_build_target),$(.VARIABLES))
$(package)_profile_env:=CARGO_PROFILE_RELEASE_OPT_LEVEL=3 CARGO_PROFILE_RELEASE_DEBUG=false
$(package)_profile_env+=CARGO_PROFILE_RELEASE_LTO=false CARGO_PROFILE_RELEASE_CODEGEN_UNITS=16
$(package)_profile_env+=CARGO_PROFILE_RELEASE_PANIC=unwind CARGO_PROFILE_RELEASE_INCREMENTAL=false
$(package)_profile_env+=CARGO_PROFILE_RELEASE_STRIP=false CARGO_INCREMENTAL=0

# RUSTFLAGS takes the place of Platform's .cargo/config.toml, which the bundle
# does not carry. Of what that file sets, --cfg tokio_unstable only enables
# tokio APIs (runtime metrics, task hooks, io-uring) that nothing in
# dash-sdk-cxx's dependency graph uses, target-feature=-crt-static only
# changes musl targets (glibc targets link the C runtime dynamically anyway),
# and target-cpu=x86-64 and -lstdc++ are the defaults or only matter for
# executables; none of them is set here.
#
# For the host, the depends compiler links (through rustc-linker.sh), the C and
# C++ sources in the crate closure (ring, blst, blake3, secp256k1, the cxx
# bridge) are compiled with the depends compiler and flags, and the build
# directory is remapped out of both the Rust and the C objects. Build scripts
# and proc macros run on the build machine; build-linker.sh links them with the
# depends build compiler, which also compiles any C they need (CC_<build
# triple> and HOST_CC; when the build and host triples are the same, the host's
# CC_<triple> wins and the host and build compilers are the same machine's).
#
# For windows-gnu, rustc creates the import libraries of raw-dylib imports
# (windows-link, which the Rust standard library and windows-sys use) with
# the mingw-w64 dlltool.
define $(package)_set_vars
$(package)_cargo_env = CARGO_HOME=$$($(package)_build_dir)/.cargo-home
$(package)_cargo_env += CARGO_TARGET_DIR=$$($(package)_build_dir)/target
$(package)_cargo_env += $($(package)_profile_env)
$(package)_cargo_env += $($(package)_build_linker_var)=$$($(package)_build_dir)/build-linker.sh
$(package)_cargo = cd / && env $$(addprefix -u ,$$($(package)_unset_env)) $$($(package)_cargo_env) \
  cargo --config $$($(package)_build_dir)/.cargo/config.toml
$(package)_rustflags = -C linker=$$($(package)_build_dir)/rustc-linker.sh --remap-path-prefix=$(BASEDIR)=/build
$(package)_rustflags_mingw32 = -C dlltool=$(host_toolchain)dlltool
$(package)_build_env += RUSTC="$(build_prefix)/bin/rustc" RUSTC_WRAPPER=
$(package)_build_env += RUSTFLAGS="$$($(package)_rustflags) $$($(package)_rustflags_$(host_os))"
$(package)_build_env += DEPENDS_CC="$$($(package)_cc)" DEPENDS_LDFLAGS="$$($(package)_ldflags)"
$(package)_build_env += DEPENDS_BUILD_CC="$(build_CC)"
$(package)_build_env += CC_$($(package)_cc_target)="$$($(package)_cc)" CXX_$($(package)_cc_target)="$$($(package)_cxx)"
$(package)_build_env += AR_$($(package)_cc_target)="$$($(package)_ar)"
$(package)_build_env += CFLAGS_$($(package)_cc_target)="$$($(package)_cppflags) $$($(package)_cflags) -ffile-prefix-map=$(BASEDIR)=/build"
$(package)_build_env += CXXFLAGS_$($(package)_cc_target)="$$($(package)_cppflags) $$($(package)_cxxflags) -ffile-prefix-map=$(BASEDIR)=/build"
$(package)_build_env += HOST_CC="$(build_CC)" HOST_CXX="$(build_CXX)"
ifneq ($(native_rust_build_target),$($(package)_rust_target))
$(package)_build_env += CC_$(subst -,_,$(native_rust_build_target))="$(build_CC)" CXX_$(subst -,_,$(native_rust_build_target))="$(build_CXX)"
endif
$(package)_build_env += PROTOC="$(build_prefix)/bin/protoc" PROTOC_INCLUDE="$(build_prefix)/include"
ifeq ($(host_os),darwin)
$(package)_build_env += MACOSX_DEPLOYMENT_TARGET=$(OSX_MIN_VERSION)
ifneq ($(build_os),darwin)
$(package)_build_env += SDKROOT="$(OSX_SDK)"
endif
endif
endef

# tenderdash-proto's build script extracts its sources from
# tenderdash-$$TENDERDASH_COMMITISH.zip in its cache directory
# (CARGO_TARGET_DIR) and would download the archive if it were missing; the
# bundle's configuration sets the tag of the archive the bundle carries.
define $(package)_preprocess_cmds
  for config in /.cargo/config /.cargo/config.toml; do \
    if test -e $$$$config; then echo "$$$$config would configure Cargo; remove it" >&2; exit 1; fi; \
  done && \
  cp $($(package)_patch_dir)/rustc-linker.sh $($(package)_patch_dir)/build-linker.sh . && \
  chmod +x rustc-linker.sh build-linker.sh && \
  tag=$$$$(sed -n 's/^TENDERDASH_COMMITISH = "\(.*\)"$$$$/\1/p' .cargo/config.toml) && \
  if ! test -f "tenderdash/tenderdash-$$$$tag.zip"; then \
    echo "the crate bundle has no Tenderdash archive for TENDERDASH_COMMITISH \"$$$$tag\"" >&2; exit 1; \
  fi && \
  mkdir -p target && \
  cp "tenderdash/tenderdash-$$$$tag.zip" target/
endef

# The build fails if the dependency graph (normal and build dependencies, both
# of which Cargo compiles) reaches a trusted third-party context provider, an
# HTTP client or OpenSSL: every trust input comes from Core. The graph goes to
# a file first, so that a failing cargo tree fails the build.
#
# The crate is built as a static library only, with rustc printing the system
# libraries the archive needs; check-native-libs.sh compares them with the
# vetted and ignored lists above. The build log goes to a file as well, and
# the exit status to another, since a pipeline has the status of tee.
define $(package)_build_cmds
  ( $($(package)_cargo) tree --frozen --manifest-path $($(package)_build_dir)/Cargo.toml \
      -p dash-sdk-cxx -e normal,build --target $($(package)_rust_target) ) > cargo-tree.txt && \
  if grep -E 'rs-sdk-trusted-context-provider|reqwest|openssl-sys' cargo-tree.txt; then \
    echo "dash-sdk-cxx depends on a forbidden crate" >&2; exit 1; \
  fi && \
  ( ( $($(package)_cargo) rustc --frozen --offline --release --manifest-path $($(package)_build_dir)/Cargo.toml \
      -p dash-sdk-cxx --lib --crate-type staticlib --target $($(package)_rust_target) \
      -- --print=native-static-libs ); echo $$$$? > cargo-rustc.status ) 2>&1 | tee cargo-rustc.log && \
  test "$$$$(cat cargo-rustc.status)" = 0 && \
  sh $($(package)_patch_dir)/check-native-libs.sh cargo-rustc.log \
    "$($(package)_native_libs_$(host_os))" "$($(package)_native_libs_ignored_$(host_os))"
endef

# The crate's build script stages the generated bridge header dash/sdk/ffi.h,
# dash/sdk/callbacks.h and the cxx runtime header rust/cxx.h under
# target/<triple>/release/include; that tree, the static archive and
# dash_sdk_cxx.pc are the installed interface. The
# .pc file locates the prefix from its own directory.
define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/include $($(package)_staging_prefix_dir)/lib/pkgconfig && \
  cp -R target/$($(package)_rust_target)/release/include/. $($(package)_staging_prefix_dir)/include/ && \
  cp target/$($(package)_rust_target)/release/libdash_sdk_cxx.a $($(package)_staging_prefix_dir)/lib/ && \
  printf '%s\n' \
    'prefix=$$$${pcfiledir}/../..' \
    'exec_prefix=$$$${prefix}' \
    'libdir=$$$${prefix}/lib' \
    'includedir=$$$${prefix}/include' \
    '' \
    'Name: dash_sdk_cxx' \
    'Description: C++ binding of the Dash Platform SDK' \
    'Version: $($(package)_version)' \
    'Cflags: -I$$$${includedir}' \
    'Libs: -L$$$${libdir} -ldash_sdk_cxx $($(package)_native_libs_$(host_os))' \
    > $($(package)_staging_prefix_dir)/lib/pkgconfig/dash_sdk_cxx.pc
endef
