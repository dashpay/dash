package=blst
$(package)_version=0.3.17
$(package)_download_path=https://github.com/supranational/blst/archive/refs/tags
$(package)_download_file=v$($(package)_version).tar.gz
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=c3fef37b566b67419703b2bbb648e15176276af9a880c22aa8f5f2e8cecc2e4d

# build.sh takes CC/AR/RANLIB/CFLAGS from the environment. __BLST_PORTABLE__
# selects runtime CPU dispatch instead of letting build.sh probe the builder's
# /proc/cpuinfo for ADX, which keeps the build reproducible; build.sh itself
# adds -D__BLST_NO_ASM__ on hosts other than x86_64/aarch64.
define $(package)_set_vars
  $(package)_build_env=CC="$($(package)_cc)" AR="$($(package)_ar)" RANLIB="$($(package)_ranlib)" CFLAGS="$($(package)_cflags) $($(package)_cppflags) -fno-builtin -fPIC -D__BLST_PORTABLE__"
endef

define $(package)_build_cmds
  ./build.sh
endef

define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/include $($(package)_staging_prefix_dir)/lib && \
  install -m 644 bindings/blst.h bindings/blst_aux.h $($(package)_staging_prefix_dir)/include && \
  install -m 644 libblst.a $($(package)_staging_prefix_dir)/lib
endef
