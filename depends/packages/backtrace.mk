package=backtrace
$(package)_version=b9e40069c0b47a722286b94eb5231f7f05c08713
$(package)_download_path=https://github.com/ianlancetaylor/libbacktrace/archive
$(package)_download_file=$($(package)_version).tar.gz
$(package)_file_name=$(package)_$($(package)_version).tar.gz
$(package)_sha256_hash=81b37e762965c676b3316e90564c89f6480606add446651c785862571a1fdbca

define $(package)_set_vars
$(package)_config_opts=--disable-shared --enable-host-shared --prefix=$(host_prefix)
endef

define $(package)_config_cmds
  cp -f $(BASEDIR)/config.guess $(BASEDIR)/config.sub . && \
  $($(package)_autoconf)
endef

define $(package)_build_cmds
  $(MAKE)
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install
endef

define $(package)_postprocess_cmds
  rm lib/*.la
endef
