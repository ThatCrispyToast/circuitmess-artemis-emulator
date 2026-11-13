# Espressif's QEMU fork plus the Artemis Watch peripheral models in ../qemu.
{
  lib,
  stdenv,
  fetchFromGitHub,
  fetchFromGitLab,
  python3Packages,
  ninja,
  meson,
  pkg-config,
  flex,
  bison,
  perl,
  glib,
  pixman,
  libgcrypt,
  zlib,
  libslirp,
}:

let
  version = "9.2.2-20260417";

  keycodemapdb = fetchFromGitLab {
    owner = "qemu-project";
    repo = "keycodemapdb";
    rev = "f5772a62ec52591ff6870b7e8ef32482371f22c6";
    hash = "sha256-GbZ5mrUYLXMi0IX4IZzles0Oyc095ij2xAsiLNJwfKQ=";
  };

  # Overlay files sit on top of the fork's tree, then the patches wire them in.
  applyOverlay = ''
    cp -r --no-preserve=mode,ownership ${../qemu/overlay}/. .
    for p in ${../qemu/patches}/*.patch; do
      patch -p1 --forward --no-backup-if-mismatch < "$p"
    done
  '';

  common = {
    inherit version;

    src = fetchFromGitHub {
      owner = "espressif";
      repo = "qemu";
      rev = "esp-develop-${version}";
      hash = "sha256-Ts7bpags6Srt3cX2l5LfBE5nPzwpXIe9UYXKBmJ+flw=";
    };

    nativeBuildInputs = [
      python3Packages.python
      python3Packages.distlib
      meson
      ninja
      pkg-config
      flex
      bison
      perl
    ];

    buildInputs = [
      glib
      pixman
      libgcrypt
      zlib
      libslirp
    ];

    # QEMU's configure drives meson itself.
    dontUseMesonConfigure = true;

    postPatch = ''
      cp -r --no-preserve=mode,ownership ${keycodemapdb} subprojects/keycodemapdb
      # The softfloat tests are the only reason to fetch the berkeley-* subprojects.
      sed -i "/subdir('fp')/d" tests/meson.build
      # The fork asks pkg-config for a static libslirp, which nixpkgs doesn't ship.
      sed -i "/slirp_dep = dependency/,+2 s/static: true/static: false/" meson.build
    '';

    preConfigure = ''
      unset CPP
      chmod +x ./scripts/shaderinclude.py
      patchShebangs .
    '';

    configureFlags = [
      "--prefix=/"
      "--target-list=xtensa-softmmu"
      "--without-default-features"
      "--disable-download"
      "--disable-docs"
      "--disable-tools"
      "--disable-guest-agent"
      "--disable-werror"
      "--disable-debug-info"
      "--enable-tcg"
      "--enable-gcrypt"
      "--enable-pixman"
      "--enable-slirp"
    ];

    preBuild = "cd build";

    enableParallelBuilding = true;

    installPhase = ''
      runHook preInstall
      DESTDIR=$out ninja install
      runHook postInstall
    '';

    meta = {
      description = "Espressif QEMU with CircuitMess Artemis Watch 2.0 peripherals";
      license = lib.licenses.gpl2Plus;
      platforms = lib.platforms.linux;
      mainProgram = "qemu-system-xtensa";
    };
  };

  final = stdenv.mkDerivation (common // {
    pname = "qemu-artemis";
    postPatch = common.postPatch + applyOverlay;
  });

in
{
  inherit final;
}
