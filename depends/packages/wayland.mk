package := wayland
native_package := native_wayland_scanner
$(package)_version := $($(native_package)_version)
$(package)_download_path := $($(native_package)_download_path)
$(package)_file_name := $($(native_package)_file_name)
$(package)_sha256_hash := $($(native_package)_sha256_hash)
$(package)_dependencies := $(native_package) libffi expat

define $(package)_set_vars
  $(package)_config_opts := --enable-option-checking --disable-dependency-tracking
  $(package)_config_opts += --enable-shared --disable-static --disable-documentation
  $(package)_config_opts += --disable-dtd-validation --with-host-scanner
  $(package)_cflags += -D_GNU_SOURCE
endef

define $(package)_config_cmds
  ac_cv_have_decl_CLOCK_MONOTONIC=yes $($(package)_autoconf)
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
