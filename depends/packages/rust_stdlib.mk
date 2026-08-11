# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# To update the Rust stdlib, change the version below and then run the script
# ./contrib/devtools/update-rust-hashes.py

package:=rust_stdlib
$(package)_version:=1.92.0
$(package)_download_path:=https://static.rust-lang.org/dist
$(package)_dependencies:=native_rust
$(package)_target=$(or \
  $($(package)_target_$(canonical_host)),\
  $($(package)_target_$(subst -pc-,-unknown-,$(canonical_host))),\
  $($(package)_target_$(subst -unknown-,-pc-,$(canonical_host))),\
  $($(package)_target_$(subst -linux-,-unknown-linux-,$(canonical_host))),\
  $(if $(findstring -apple-darwin,$(canonical_host)),$(host_arch)-apple-darwin))

# Android
$(package)_targets += aarch64-linux-android
$(package)_targets += armv7-linux-androideabi
$(package)_targets += i686-linux-android
$(package)_targets += x86_64-linux-android
$(package)_target_aarch64-unknown-linux-android:=aarch64-linux-android
$(package)_target_armv7a-unknown-linux-android:=armv7-linux-androideabi
$(package)_target_i686-pc-linux-android:=i686-linux-android
$(package)_target_x86_64-pc-linux-android:=x86_64-linux-android
$(package)_sha256_hash_aarch64-linux-android:=f6689cf5b71056e887261ec84ae1f499eeb42c67e5ae73e7c0e06065b6648c44
$(package)_sha256_hash_armv7-linux-androideabi:=18f6e6903c5f4361efe8c8a1ea546303e4473e8b9cd1b11fcf4e5b170468d464
$(package)_sha256_hash_i686-linux-android:=d994d70493ad68ced22a5269e41b9fa6f83af9f3adabade154860058bc2e18c0
$(package)_sha256_hash_x86_64-linux-android:=b9074f6961baff09334afaff745fb16dbf9f05cdf856b17309d8e6232a281c40

# FreeBSD (x86_64)
$(package)_targets += x86_64-unknown-freebsd
$(package)_target_x86_64-unknown-freebsd:=x86_64-unknown-freebsd
$(package)_sha256_hash_x86_64-unknown-freebsd:=3616afb808cd030e65e66c51f6f0fb6a6fc52d877d0d70bb681b0c35238adbe8

# Linux (ARMv7)
$(package)_targets += armv7-unknown-linux-musleabihf
$(package)_target_arm-unknown-linux-gnueabihf:=armv7-unknown-linux-musleabihf
$(package)_target_armv7-unknown-linux-gnueabihf:=armv7-unknown-linux-musleabihf
$(package)_sha256_hash_armv7-unknown-linux-musleabihf:=fc5c4ca757599caab8e93000becb9d57587088d32dab5c4f3b253f00ec3a2fd6

# Linux (ARMv8)
$(package)_targets += aarch64-unknown-linux-musl
$(package)_target_aarch64-unknown-linux-gnu:=aarch64-unknown-linux-musl
$(package)_sha256_hash_aarch64-unknown-linux-musl:=715fbcfd8712c723947a020d0371c8a1a21f7531f2b696aeaed50ac23ba675c9

# Linux (x86 32-bit)
$(package)_targets += i686-unknown-linux-musl
$(package)_target_i686-pc-linux-gnu:=i686-unknown-linux-musl
$(package)_target_i686-unknown-linux-gnu:=i686-unknown-linux-musl
$(package)_sha256_hash_i686-unknown-linux-musl:=3d6ccb700a17533eea10c7541896c92817783045c5537af37228142da7668fb3

# Linux (PowerPC 64-bit little-endian)
$(package)_targets += powerpc64le-unknown-linux-musl
$(package)_target_powerpc64le-unknown-linux-gnu:=powerpc64le-unknown-linux-musl
$(package)_sha256_hash_powerpc64le-unknown-linux-musl:=696958d87842d877640140ddbbaa74d044374874dee9516e227a395b70bfb8d4

# Linux (PowerPC 64-bit big-endian)
$(package)_targets += powerpc64-unknown-linux-gnu
$(package)_target_powerpc64-unknown-linux-gnu:=powerpc64-unknown-linux-gnu
$(package)_sha256_hash_powerpc64-unknown-linux-gnu:=c47938c152f1b237c901090d69522ce1ccfa69a859ed10d634fe3083108c3017

# Linux (RISCV64GC)
$(package)_targets += riscv64gc-unknown-linux-musl
$(package)_target_riscv64-unknown-linux-gnu:=riscv64gc-unknown-linux-musl
$(package)_target_riscv64gc-unknown-linux-gnu:=riscv64gc-unknown-linux-musl
$(package)_sha256_hash_riscv64gc-unknown-linux-musl:=34f5722ff2a0940bcd7ff6603a7748d2b963de72f6f713579c39c74ead06a7a0

# Linux (x86_64)
$(package)_targets += x86_64-unknown-linux-musl
$(package)_target_x86_64-unknown-linux-gnu:=x86_64-unknown-linux-musl
$(package)_sha256_hash_x86_64-unknown-linux-musl:=8bfd9a42c8295949d556587201acdb35d2bfb8b7ce55223845f337aa5614f9a3

# macOS (ARMv8)
$(package)_targets += aarch64-apple-darwin
$(package)_target_aarch64-apple-darwin:=aarch64-apple-darwin
$(package)_target_arm64-apple-darwin:=aarch64-apple-darwin
$(package)_sha256_hash_aarch64-apple-darwin:=b1f55aac4bc982ea67b68b262b711263005e470d31cab5d09d534bc1866d455a

# macOS (x86_64)
$(package)_targets += x86_64-apple-darwin
$(package)_target_x86_64-apple-darwin:=x86_64-apple-darwin
$(package)_sha256_hash_x86_64-apple-darwin:=1e5a8fee4e038ea2d35d82a680e2b9bf44ffccb3746aaf9dbdc56cb14152dcb8

# Windows (x86_64)
$(package)_targets += x86_64-pc-windows-gnu
$(package)_target_x86_64-w64-mingw32:=x86_64-pc-windows-gnu
$(package)_sha256_hash_x86_64-pc-windows-gnu:=6256f3497e3b14b6650511e84fdfb51fc632db1908ae5a173dffcdc96c80b7ce

$(package)_file_name=rust-std-$($(package)_version)-$($(package)_target).tar.gz
$(package)_sha256_hash=$($(package)_sha256_hash_$($(package)_target))

define $(package)_fetch_cmds
  $(call fetch_file,$(package),$($(package)_download_path),$($(package)_file_name),$($(package)_file_name),$($(package)_sha256_hash))
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib && \
  cp -r rust-std-$($(package)_target)/lib/rustlib/$($(package)_target) $($(package)_staging_dir)/$(host_prefix)/native/lib/rustlib/
endef
