# CircuitMess's MicroPython for their ESP32-S3 devices, the Artemis Watch among them.
# They don't publish a binary, so it's built from their source using the ESP-IDF
# release and compiler it supports. The board is the "combined" one they keep
# current, which carries every device's library and a 2 MB filesystem.
{
  lib,
  stdenv,
  fetchFromGitHub,
  fetchurl,
  autoPatchelfHook,
  cmake,
  ninja,
  git,
  gnumake,
  python312,
  zlib,
}:

let
  toolchain = stdenv.mkDerivation {
    pname = "xtensa-esp32s3-elf";
    version = "esp-2022r1-11.2.0";
    # URL and hash from tools/tools.json in ESP-IDF v5.0.2.
    src = fetchurl {
      url = "https://github.com/espressif/crosstool-NG/releases/download/esp-2022r1/xtensa-esp32s3-elf-gcc11_2_0-esp-2022r1-linux-amd64.tar.xz";
      sha256 = "5058b2e724166c34ca09ec2d5377350252de8bce5039b06c00352f9a8151f76e";
    };
    nativeBuildInputs = [ autoPatchelfHook ];
    buildInputs = [
      stdenv.cc.cc.lib
      zlib
    ];
    # The toolchain's GDB wants a Python that isn't needed to compile anything.
    autoPatchelfIgnoreMissingDeps = true;
    dontConfigure = true;
    dontBuild = true;
    dontStrip = true;
    installPhase = "cp -r . $out";
  };

  esp-idf = fetchFromGitHub {
    owner = "espressif";
    repo = "esp-idf";
    rev = "v5.0.2";
    fetchSubmodules = true;
    hash = "sha256-dlmtTjoz4qQdFG229v9bIHKpYBzjM44fv+XhdDBu2Os=";
  };

  # The component manager needs network access, so its one dependency is pinned here.
  esp-protocols = fetchFromGitHub {
    owner = "espressif";
    repo = "esp-protocols";
    rev = "mdns-v1.1.0";
    hash = "sha256-IfjXipy6RPyuzXSkEo9T9GCHucC/gfMpd7U9G+ilPHk=";
  };

  berkeley-db = fetchFromGitHub {
    owner = "pfalcon";
    repo = "berkeley-db-1.xx";
    rev = "35aaec4418ad78628a3b935885dd189d41ce779b";
    hash = "sha256-XItxmpXXPgv11LcnL7dty6uq1JctGokHCU8UGG9ic04=";
  };

  micropython-lib = fetchFromGitHub {
    owner = "micropython";
    repo = "micropython-lib";
    rev = "c113611765278b2fc8dcf8b2f2c3513b35a69b39";
    hash = "sha256-Bh21HxOUVdFbbtAPTr3I5krhS5kIqBbMleUCodg3hBo=";
  };

  st7789_mpy = fetchFromGitHub {
    owner = "CircuitMess";
    repo = "st7789_mpy";
    rev = "97008c65fba7b24acb388e48bdc802016c1fea69";
    hash = "sha256-4zkatXFRpRLzUQd6Hsmcj0xttFTdMc9yuzriiA7acmk=";
  };

  # ESP-IDF expects this to be pip-installed. The esptool.py inside ESP-IDF is only a
  # shim for `python -m esptool`, so its directory must never be on the Python path:
  # the shim would find itself and re-run itself until the machine runs out of memory.
  esptool = fetchFromGitHub {
    owner = "espressif";
    repo = "esptool";
    rev = "v4.5.1";
    hash = "sha256-FKFw7czXzC8F3OXjlLoJEFaqsSgqWz0ZEqd7KjCy5Ik=";
  };

  python = python312.withPackages (ps: [
    ps.setuptools
    ps.click
    ps.pyserial
    ps.future
    ps.cryptography
    ps.pyparsing
    ps.pyelftools
    ps.kconfiglib
    ps.bitstring
    ps.reedsolo
    ps.intelhex
    ps.packaging
    ps.pyyaml
  ]);
in
stdenv.mkDerivation {
  pname = "micropython-circuitmess-s3";
  version = "02bf648";

  src = fetchFromGitHub {
    owner = "CircuitMess";
    repo = "micropython";
    rev = "02bf6480cafe80b1c2ccfe420079bd83d9415c87";
    hash = "sha256-XOF7gqZBSgDrx5hLiRPd5BNSX4SZmjou+4JAmo+J520=";
  };

  nativeBuildInputs = [
    cmake
    ninja
    git
    gnumake
    python
    toolchain
  ];

  dontUseCmakeConfigure = true;
  dontUseNinjaBuild = true;
  dontUseNinjaInstall = true;
  dontFixup = true;

  # The ESP-IDF build system expects to find a compiler for the target only.
  hardeningDisable = [ "all" ];

  postPatch = ''
    cp -r --no-preserve=mode ${berkeley-db}/. lib/berkeley-db-1.xx/
    cp -r --no-preserve=mode ${micropython-lib}/. lib/micropython-lib/
    cp -r --no-preserve=mode ${st7789_mpy}/. usermod/st7789_mpy/

    mkdir ../esp-idf
    cp -r --no-preserve=mode ${esp-idf}/. ../esp-idf/
    chmod -R u+x ../esp-idf/tools ../esp-idf/components/esptool_py/esptool
    echo "v5.0.2" > ../esp-idf/version.txt
    patchShebangs ../esp-idf/tools ../esp-idf/components/esptool_py/esptool

    # This directive is newer than the compiler that matches this ESP-IDF release.
    sed -i -E 's/^#elifdef (.*)$/#elif defined(\1)/' ports/esp32/cm_boards.c

    # mDNS comes from the component registry upstream; see esp-protocols above.
    cp -r --no-preserve=mode ${esp-protocols}/components/mdns ../mdns
    rm -f ports/esp32/main_esp32s3/idf_component.yml ../mdns/idf_component.yml
    sed -i 's/^    efuse$/    efuse\n    mdns/' ports/esp32/esp32_common.cmake
  '';

  configurePhase = ''
    runHook preConfigure

    # A runaway chain of subprocesses should fail the build, not exhaust the machine.
    ulimit -u 1024

    export IDF_PATH=$(realpath ../esp-idf)
    export IDF_COMPONENT_MANAGER=0
    export IDF_SKIP_CHECK_SUBMODULES=1
    export IDF_TARGET=esp32s3
    export ESP_ROM_ELF_DIR=$IDF_PATH
    export CMAKE_POLICY_VERSION_MINIMUM=3.5
    export HOME=$TMPDIR
    mkdir -p $TMPDIR/pylib
    ln -s ${esptool}/esptool $TMPDIR/pylib/esptool
    export PYTHONPATH=$TMPDIR/pylib

    # This MicroPython predates the warnings of the host compiler used for its tools.
    make -C mpy-cross -j$NIX_BUILD_CORES CWARN=-w

    cd ports/esp32
    cmake -S . -B build -G Ninja \
      -DMICROPY_BOARD=CM_-CombinedS3 \
      -DUSER_C_MODULES=$(realpath ../../usermod/micropython.cmake) \
      -DPYTHON_DEPS_CHECKED=1 \
      -DPYTHON=python3 \
      -DEXTRA_COMPONENT_DIRS=$(realpath ../../../mdns)
    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild
    ninja -C build -j$NIX_BUILD_CORES
    python3 makeimg.py build/sdkconfig build/bootloader/bootloader.bin \
      build/partition_table/partition-table.bin build/micropython.bin \
      build/firmware.bin build/micropython.uf2
    runHook postBuild
  '';

  installPhase = ''
    mkdir -p $out
    cp build/firmware.bin $out/firmware.bin
    cp build/micropython.elf $out/micropython.elf
  '';

  meta = {
    description = "CircuitMess MicroPython firmware for their ESP32-S3 devices";
    license = lib.licenses.mit;
  };
}
