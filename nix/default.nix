{
  lib,
  stdenv,
  cmake,
  pkg-config,
  kdePackages,
  polkit,
  qt6,
  gcr_4,
  glib,
  json-glib,
  version ? "0",
}: let
  inherit (lib.sources) cleanSource cleanSourceWith;
  inherit (lib.strings) hasSuffix;
in
  stdenv.mkDerivation {
    pname = "bb-auth";
    inherit version;

    src = cleanSourceWith {
      filter = name: _type: let
        baseName = baseNameOf (toString name);
      in
        ! (hasSuffix ".nix" baseName);
      src = cleanSource ../.;
    };

    nativeBuildInputs = [
      cmake
      pkg-config
      qt6.wrapQtAppsHook
    ];

    buildInputs = [
      polkit
      kdePackages.polkit-qt-1
      qt6.qtbase
      # For keyring-prompter
      gcr_4
      glib
      json-glib
    ];

    meta = {
      description = "Unified polkit, keyring, and pinentry authentication daemon";
      homepage = "https://github.com/anthonyhab/bb-auth";
      license = lib.licenses.bsd3;
      maintainers = [lib.maintainers.fufexan];
      mainProgram = "bb-auth";
      platforms = lib.platforms.linux;
    };
  }
