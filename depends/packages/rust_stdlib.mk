# Copyright (c) 2016-2025 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Precompiled Rust standard library for the host, installed next to the
# native_rust compiler. The version follows native_rust.mk; update both with
# ./contrib/devtools/update-rust-hashes.py

package:=rust_stdlib
$(package)_version:=$(native_rust_version)
$(package)_download_path:=$(native_rust_download_path)

# Every host in contrib/guix/guix-build's default HOSTS has an entry. Linux
# hosts use the glibc (-unknown-linux-gnu) standard library, the one Rust
# supports for linking into a glibc program. Its libc imports are unversioned
# in the archive and bind at link time to the glibc the program is linked
# against; every symbol it requires unconditionally exists in glibc 2.31 on
# all five Linux architectures (the rest are weak and looked up at run time).

# Linux (x86_64)
$(package)_target_x86_64_linux:=x86_64-unknown-linux-gnu
$(package)_sha256_hash_x86_64_linux:=eddab0358cbd12aeb897716aab00d1db7b59696e85b9ac4982e72259a9a976b1

# Linux (ARMv8)
$(package)_target_aarch64_linux:=aarch64-unknown-linux-gnu
$(package)_sha256_hash_aarch64_linux:=779407b14507542581216d89eb9f3fbb232abbf3abcc15c365cb32fa0614e409

# Linux (RISC-V 64)
$(package)_target_riscv64_linux:=riscv64gc-unknown-linux-gnu
$(package)_sha256_hash_riscv64_linux:=bea4eac8f0b752aec63389d626d96280424da68b033c2d515bc4af204f07bf44

# Linux (ARMv7, hard float)
$(package)_target_arm_linux:=armv7-unknown-linux-gnueabihf
$(package)_sha256_hash_arm_linux:=6f15060d308793d1687a5092c80f2fbebc808c73096980b67f9de71d4f54f92c

# Linux (POWER, big endian)
$(package)_target_powerpc64_linux:=powerpc64-unknown-linux-gnu
$(package)_sha256_hash_powerpc64_linux:=2d6268b4dddc385c24ae77b2e1fe16161781b92958108cbb2a6f9fab21689823

# Windows (x86_64)
$(package)_target_x86_64_mingw32:=x86_64-pc-windows-gnu
$(package)_sha256_hash_x86_64_mingw32:=0cda26447df0749bc84044be8c8083ac4dc87bf137c11cc31ec9673a6e2e0344

# macOS (x86_64)
$(package)_target_x86_64_darwin:=x86_64-apple-darwin
$(package)_sha256_hash_x86_64_darwin:=af7ffb3b408aa2f6a6940fc83ea6dc9c3e919d18f1b04f1a581b7896441e8b78

# macOS (ARMv8)
$(package)_target_aarch64_darwin:=aarch64-apple-darwin
$(package)_sha256_hash_aarch64_darwin:=840484e8f9c2a8ed024b706262a1257bb07d9617670a1fc90020536282950690

$(package)_target:=$($(package)_target_$(host_arch)_$(host_os))
$(package)_sha256_hash:=$($(package)_sha256_hash_$(host_arch)_$(host_os))

ifeq ($($(package)_target),)
$(error rust_stdlib has no Rust standard library for $(host))
endif

$(package)_file_name:=rust-std-$($(package)_version)-$($(package)_target).tar.gz

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_dir)$(build_prefix)/lib/rustlib && \
  cp -R rust-std-$($(package)_target)/lib/rustlib/$($(package)_target) $($(package)_staging_dir)$(build_prefix)/lib/rustlib/
endef
