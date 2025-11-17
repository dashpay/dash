let
  pkgsLegacy = import (builtins.fetchTarball {
    name = "nixos-23.11";
    url = "https://github.com/NixOS/nixpkgs/archive/23.11.tar.gz";
    sha256 = "sha256-MxCVrXY6v4QmfTwIysjjaX0XUhqBbxTWWB4HXtDYsdk=";
  }) {};

  pkgsUnstable = import (builtins.fetchTarball {
    name = "nixos-unstable";
    url = "https://github.com/NixOS/nixpkgs/archive/85a6c4a07faa12aaccd81b36ba9bfc2bec974fa1.tar.gz";
    sha256 = "sha256-3YJkOBrFpmcusnh7i8GXXEyh7qZG/8F5z5+717550Hk=";
  }) {};
in
pkgsUnstable.buildEnv {
  name = "nixEnv";
  ignoreCollisions = true;
  paths = [
    pkgsLegacy.gcc11
    pkgsUnstable.gcc15
  ];

  postBuild = ''
    binaries=(
      addr2line
      ar
      as
      c++
      c++filt
      cc
      cpp
      dwp
      elfedit
      g++
      gcc
      gcc-ar
      gcc-nm
      gcc-ranlib
      gccgo
      gcov
      gcov-dump
      gcov-tool
      gdc
      gfortran
      gprof
      ld
      ld.bfd
      ld.gold
      lto-dump
      nm
      objcopy
      objdump
      ranlib
      readelf
      size
      strings
      strip
    )

    target_triple="${pkgsUnstable.stdenv.targetPlatform.config}"
    for binary in "''${binaries[@]}"; do
      if [ -f "${pkgsLegacy.gcc11}/bin/$binary" ]; then
        ln -sf "${pkgsLegacy.gcc11}/bin/$binary" "$out/bin/$binary-11"
        ln -sf "${pkgsLegacy.gcc11}/bin/$binary" "$out/bin/$target_triple-$binary-11"
      fi
      if [ -f "${pkgsUnstable.gcc15}/bin/$binary" ]; then
        ln -sf "${pkgsUnstable.gcc15}/bin/$binary" "$out/bin/$binary-15"
        ln -sf "${pkgsUnstable.gcc15}/bin/$binary" "$out/bin/$target_triple-$binary-15"
      fi
      rm -f "$out/bin/$binary"
    done

    mkdir -p $out/lib $out/lib64
    if [ -d "${pkgsLegacy.gcc11.cc.lib}/lib" ]; then
      ln -sf "${pkgsLegacy.gcc11.cc.lib}/lib" "$out/lib/gcc-11"
    fi
    if [ -d "${pkgsLegacy.gcc11.cc.lib}/lib64" ]; then
      ln -sf "${pkgsLegacy.gcc11.cc.lib}/lib64" "$out/lib64/gcc-11"
    fi
    if [ -d "${pkgsUnstable.gcc15.cc.lib}/lib" ]; then
      ln -sf "${pkgsUnstable.gcc15.cc.lib}/lib" "$out/lib/gcc-15"
    fi
    if [ -d "${pkgsUnstable.gcc15.cc.lib}/lib64" ]; then
      ln -sf "${pkgsUnstable.gcc15.cc.lib}/lib64" "$out/lib64/gcc-15"
    fi
  '';
}
