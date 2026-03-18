{
  description = "WEF Webview Backend";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils, wefInclude ? null }:
    flake-utils.lib.eachSystem [ "x86_64-darwin" "aarch64-darwin" "x86_64-linux" "aarch64-linux" ] (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};

        isDarwin = pkgs.stdenv.isDarwin;
        isLinux = pkgs.stdenv.isLinux;

        darwinBuildInputs = if isDarwin then [
          pkgs.apple-sdk_15
        ] else [];

        linuxBuildInputs = if isLinux then
          (with pkgs; [ webkitgtk_4_1 gtk3 glib ])
        else [];

        buildInputs = darwinBuildInputs ++ linuxBuildInputs;

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
        ] ++ (if isLinux then [ pkg-config ] else [])
          ++ (if isDarwin then [ pkgs.darwin.sigtool ] else []);

      in {
        packages = {
          default = pkgs.stdenv.mkDerivation {
            pname = "wef-webview";
            version = "0.1.0";

            src = ./.;

            inherit buildInputs nativeBuildInputs;

            dontConfigure = true;

            buildPhase = ''
              runHook preBuild
              ${if wefInclude != null then ''
                mkdir -p include
                cp ${wefInclude}/wef.h include/
              '' else ""}
              cmake -G Ninja \
                -DCMAKE_BUILD_TYPE=Release \
                ${if wefInclude != null then "-DWEF_INCLUDE_DIR=$PWD/include" else ""} \
                -B build \
                .
              ninja -C build
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin

              ${if isDarwin then ''
                mkdir -p $out/Applications
                cp -r build/wef_webview.app $out/Applications/
                /usr/bin/codesign -s - --force --deep $out/Applications/wef_webview.app || true
                mkdir -p $out/bin
                ln -s $out/Applications/wef_webview.app/Contents/MacOS/wef_webview $out/bin/wef-webview
              '' else ''
                cp build/wef_webview $out/bin/wef-webview
              ''}

              runHook postInstall
            '';
          };
        };

        devShells.default = pkgs.mkShell {
          inherit buildInputs nativeBuildInputs;

          shellHook = ''
            echo "WEF Webview Development Shell"
            echo ""
            echo "Build commands:"
            echo "  cmake -G Ninja -B build && ninja -C build"
            echo ""
            echo "Run:"
            ${if isDarwin then ''
              echo "  WEF_RUNTIME_PATH=<path/to/runtime.dylib> ./build/wef_webview.app/Contents/MacOS/wef_webview"
            '' else ''
              echo "  WEF_RUNTIME_PATH=<path/to/runtime.so> ./build/wef_webview"
            ''}
          '';
        };
      }
    );
}
