{
  description = "Unified polkit, keyring, and pinentry authentication daemon";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";

  };

  outputs = {
    self,
    nixpkgs,
    systems,
    ...
  } @ inputs: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);
    pkgsFor = eachSystem (
      system:
        import nixpkgs {
          localSystem = system;
          overlays = [self.overlays.default];
        }
    );
  in {
    overlays = import ./nix/overlays.nix {inherit inputs self lib;};

    packages = eachSystem (system: {
      default = self.packages.${system}."noctalia-auth";
      inherit (pkgsFor.${system}) "noctalia-auth";
    });

    devShells = eachSystem (system: {
      default = import ./nix/shell.nix {
        pkgs = pkgsFor.${system};
        noctaliaAuth = pkgsFor.${system}."noctalia-auth";
      };
    });
  };
}
