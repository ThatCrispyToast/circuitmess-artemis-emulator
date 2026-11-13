{
  description = "Full-system emulator for the CircuitMess NASA Artemis Watch 2.0";

  inputs.nixpkgs.url = "https://releases.nixos.org/nixos/unstable/nixos-26.11pre1074753.b1b875982b17/nixexprs.tar.xz";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};

      qemu = pkgs.callPackage ./nix/qemu.nix { };

      # CircuitMess's published images aren't redistributable from here, so they're
      # fetched from their releases when something needs them. 2.0 is the first for
      # this watch, and what hardware revision 2 shipped with.
      stockReleases = {
        "2.0" = "sha256-S89iP2ItiawvY6WeeFFoAyLDSf0YjNDLTNyS9IdRMzA=";
        "2.1" = "sha256-DT9DZFbaOP/FEZsIFNG7J9L5mn7JUTjgZWM92yl5Bxk=";
        "2.1.1" = "sha256-pW7V0MqWyurHB+/qb5wzrHkkVlG9EuPNfa1OBghN3mY=";
        "2.2" = "sha256-3iyLrhbbYCzy4POhzRxe2zs41gu3L3quOcrSYeodeU4=";
      };
      stockRelease =
        version:
        pkgs.fetchurl {
          url = "https://github.com/CircuitMess/GC_Artemis-Firmware/releases/download/v${version}/Artemis-v${version}.bin";
          hash = stockReleases.${version};
        };
      stockFirmware = stockRelease "2.2";
      stockFirmwareDir = pkgs.linkFarm "artemis-stock-firmware" (
        map (version: {
          name = "Artemis-v${version}.bin";
          path = stockRelease version;
        }) (builtins.attrNames stockReleases)
      );

      micropythonFirmware = pkgs.callPackage ./nix/micropython.nix { };

      python = pkgs.python3.withPackages (ps: [
        ps.pygame-ce
        ps.pytest
        ps.pyserial
      ]);

      emulator = pkgs.stdenv.mkDerivation {
        pname = "artemis-emu";
        version = "0.1.0";
        src = pkgs.lib.fileset.toSource {
          root = ./.;
          fileset = pkgs.lib.fileset.fileFilter (
            file: file.hasExt "py" || file.name == "watch_registers.txt"
          ) ./.;
        };
        nativeBuildInputs = [ pkgs.makeWrapper ];
        installPhase = ''
          share=$out/share/artemis-emu
          mkdir -p $share $out/bin
          cp -r artemis_emu examples tests $share/
          env=(
            --set PYTHONPATH $share
            --set ARTEMIS_QEMU ${qemu.final}/bin/qemu-system-xtensa
            --set ARTEMIS_STOCK_FIRMWARE ${stockFirmware}
            --set ARTEMIS_STOCK_FIRMWARE_DIR ${stockFirmwareDir}
            --set ARTEMIS_MICROPYTHON_FIRMWARE ${micropythonFirmware}/firmware.bin
            --prefix PATH : ${pkgs.esptool}/bin
          )
          makeWrapper ${python}/bin/python $out/bin/artemis-emu \
            --add-flags "-m artemis_emu" "''${env[@]}"
          makeWrapper ${python}/bin/python $out/bin/artemis-emu-test \
            --add-flags "-m pytest -p no:cacheprovider $share/tests" "''${env[@]}"
        '';
        meta = {
          description = "Emulator for the CircuitMess NASA Artemis Watch 2.0";
          mainProgram = "artemis-emu";
          platforms = [ system ];
        };
      };
    in
    {
      packages.${system} = {
        default = emulator;
        qemu-artemis = qemu.final;
        stock-firmware = stockFirmware;
        micropython-firmware = micropythonFirmware;
      };

      apps.${system} = {
        default = {
          type = "app";
          program = "${emulator}/bin/artemis-emu";
        };
        test = {
          type = "app";
          program = "${emulator}/bin/artemis-emu-test";
        };
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = [
          python
          pkgs.esptool
        ];
        ARTEMIS_QEMU = "${qemu.final}/bin/qemu-system-xtensa";
        ARTEMIS_STOCK_FIRMWARE = "${stockFirmware}";
        ARTEMIS_STOCK_FIRMWARE_DIR = "${stockFirmwareDir}";
        ARTEMIS_MICROPYTHON_FIRMWARE = "${micropythonFirmware}/firmware.bin";
      };
    };
}
