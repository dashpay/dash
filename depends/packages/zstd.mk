package=zstd
$(package)_version=1.5.7
$(package)_download_path=https://github.com/facebook/zstd/releases/download/v$($(package)_version)
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3

define $(package)_set_vars
  $(package)_build_opts=CC="$($(package)_cc)" AR="$($(package)_ar)"
  $(package)_build_opts+=CFLAGS="$($(package)_cflags) $($(package)_cppflags)" LDFLAGS="$($(package)_ldflags)"
  $(package)_build_opts+=ZSTD_LEGACY_SUPPORT=0 ZSTD_LIB_DEPRECATED=0 ZSTD_LIB_DICTBUILDER=0
endef

define $(package)_build_cmds
  $(MAKE) -C lib libzstd.a $($(package)_build_opts)
endef

define $(package)_stage_cmds
  $(MAKE) -C lib install-static install-includes install-pc PREFIX=$(host_prefix) DESTDIR=$($(package)_staging_dir) $($(package)_build_opts)
endef
