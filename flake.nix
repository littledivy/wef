{
  description = "deno_desktop";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, rust-overlay }:
    flake-utils.lib.eachSystem [ "x86_64-darwin" "aarch64-darwin" ] (system:
      let
        overlays = [ (import rust-overlay) ];
        pkgs = import nixpkgs {
          inherit system overlays;
        };

        rustToolchain = pkgs.rust-bin.stable.latest.default;

        # Pre-fetch rusty_v8 static library
        rustyV8Version = "145.0.0";
        rustyV8Arch = if system == "aarch64-darwin" then "aarch64-apple-darwin" else "x86_64-apple-darwin";
        rustyV8 = pkgs.fetchurl {
          url = "https://github.com/denoland/rusty_v8/releases/download/v${rustyV8Version}/librusty_v8_release_${rustyV8Arch}.a.gz";
          sha256 = if system == "aarch64-darwin"
            then "sha256-yHa1eydVCrfYGgrZANbzgmmf25p7ui1VMas2A7BhG6k="
            else "sha256-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        };

        helloRuntime = pkgs.rustPlatform.buildRustPackage {
          pname = "hello_runtime";
          version = "0.1.0";
          src = ./.;
          cargoLock.lockFile = ./Cargo.lock;
          buildAndTestSubdir = "examples/hello";
          nativeBuildInputs = [ rustToolchain pkgs.llvmPackages.libclang ];
          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
          buildPhase = ''
            cargo build --release -p hello_runtime
          '';
          installPhase = ''
            mkdir -p $out/lib
            cp target/release/libhello_runtime.dylib $out/lib/
          '';
          doCheck = false;
        };

        ddcoreRuntime = pkgs.rustPlatform.buildRustPackage {
          pname = "ddcore_runtime";
          version = "0.1.0";
          src = ./.;
          cargoLock.lockFile = ./Cargo.lock;
          buildAndTestSubdir = "examples/ddcore";
          nativeBuildInputs = [ rustToolchain pkgs.llvmPackages.libclang ];
          buildInputs = [ pkgs.libiconv ];
          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";
          RUSTY_V8_ARCHIVE = rustyV8;
          buildPhase = ''
            cargo build --release -p ddcore_runtime
          '';
          installPhase = ''
            mkdir -p $out/lib
            cp target/release/libddcore_runtime.dylib $out/lib/
          '';
          doCheck = false;
        };

        # Import sub-flakes
        cefFlake = import ./cef/flake.nix;
        cefOutputs = cefFlake.outputs { self = cefFlake; inherit nixpkgs flake-utils; };
        cefApp = cefOutputs.packages.${system}.default;

        webviewFlake = import ./webview/flake.nix;
        webviewOutputs = webviewFlake.outputs { self = webviewFlake; inherit nixpkgs flake-utils; };
        webviewApp = webviewOutputs.packages.${system}.default;

        servoFlake = import ./servo/flake.nix;
        servoOutputs = servoFlake.outputs { self = servoFlake; inherit nixpkgs flake-utils rust-overlay; };
        servoApp = servoOutputs.packages.${system}.default;

      in {
        packages = {
          # Runtimes
          hello = helloRuntime;
          ddcore = ddcoreRuntime;

          # Backends
          cef = cefApp;
          webview = webviewApp;
          servo = servoApp;

          # Bundles
          cef-hello = pkgs.stdenv.mkDerivation {
            pname = "deno-cef-hello";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${cefApp}/Applications/cefsimple.app $out/Applications/
              cp ${helloRuntime}/lib/libhello_runtime.dylib $out/lib/
              cat > $out/bin/deno-cef-hello <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/cefsimple.app/Contents/MacOS/cefsimple" --runtime "$SCRIPT_DIR/lib/libhello_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/deno-cef-hello
            '';
          };

          servo-hello = pkgs.stdenv.mkDerivation {
            pname = "deno-servo-hello";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib
              cp ${servoApp}/bin/deno_servo $out/bin/
              cp ${helloRuntime}/lib/libhello_runtime.dylib $out/lib/
              cat > $out/bin/deno-servo-hello <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DD_RUNTIME_PATH="$SCRIPT_DIR/lib/libhello_runtime.dylib" exec "$SCRIPT_DIR/bin/deno_servo" "$@"
EOF
              chmod +x $out/bin/deno-servo-hello
            '';
          };

          servo-ddcore = pkgs.stdenv.mkDerivation {
            pname = "deno-servo-ddcore";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib
              cp ${servoApp}/bin/deno_servo $out/bin/
              cp ${ddcoreRuntime}/lib/libddcore_runtime.dylib $out/lib/
              cat > $out/bin/deno-servo-ddcore <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DD_RUNTIME_PATH="$SCRIPT_DIR/lib/libddcore_runtime.dylib" exec "$SCRIPT_DIR/bin/deno_servo" "$@"
EOF
              chmod +x $out/bin/deno-servo-ddcore
            '';
          };

          cef-ddcore = pkgs.stdenv.mkDerivation {
            pname = "deno-cef-ddcore";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${cefApp}/Applications/cefsimple.app $out/Applications/
              cp ${ddcoreRuntime}/lib/libddcore_runtime.dylib $out/lib/
              cat > $out/bin/deno-cef-ddcore <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/cefsimple.app/Contents/MacOS/cefsimple" --runtime "$SCRIPT_DIR/lib/libddcore_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/deno-cef-ddcore
            '';
          };

          webview-hello = pkgs.stdenv.mkDerivation {
            pname = "deno-webview-hello";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${webviewApp}/Applications/deno_webview.app $out/Applications/
              cp ${helloRuntime}/lib/libhello_runtime.dylib $out/lib/
              cat > $out/bin/deno-webview-hello <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/deno_webview.app/Contents/MacOS/deno_webview" --runtime "$SCRIPT_DIR/lib/libhello_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/deno-webview-hello
            '';
          };

          webview-ddcore = pkgs.stdenv.mkDerivation {
            pname = "deno-webview-ddcore";
            version = "0.1.0";
            dontUnpack = true;
            installPhase = ''
              mkdir -p $out/bin $out/lib $out/Applications
              cp -R ${webviewApp}/Applications/deno_webview.app $out/Applications/
              cp ${ddcoreRuntime}/lib/libddcore_runtime.dylib $out/lib/
              cat > $out/bin/deno-webview-ddcore <<'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$SCRIPT_DIR/Applications/deno_webview.app/Contents/MacOS/deno_webview" --runtime "$SCRIPT_DIR/lib/libddcore_runtime.dylib" "$@"
EOF
              chmod +x $out/bin/deno-webview-ddcore
            '';
          };

          default = helloRuntime;
        };

        devShells.default = pkgs.mkShell {
          buildInputs = [
            rustToolchain
            pkgs.libiconv
            pkgs.cmake
            pkgs.ninja
            pkgs.llvmPackages.libclang
          ] ++ (with pkgs.darwin.apple_sdk.frameworks; [ Cocoa WebKit ]);

          LIBCLANG_PATH = "${pkgs.llvmPackages.libclang.lib}/lib";

          shellHook = ''
            echo "deno_desktop dev shell"
            echo "nix build .#servo-hello"
          '';
        };
      }
    );
}
