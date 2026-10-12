{
  description = "OTPClient (Windows port fork): development environment";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      linuxSystems = [ "x86_64-linux" "aarch64-linux" ];
      forLinux = f: nixpkgs.lib.genAttrs linuxSystems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forLinux (pkgs:
        let
          # Everything the Linux build (GUI + CLI + search provider + tests) needs.
          # Mirrors the pkg_check_modules() calls in CMakeLists.txt.
          libs = with pkgs; [
            libgcrypt
            libcotp
            jansson
            glib
            libsecret
            util-linux.dev # libuuid
            gtk4
            libadwaita
            gdk-pixbuf
            zbar
            protobufc
            qrencode
          ];
          windowsPkgs = pkgs.pkgsCross.ucrt64.extend (final: prev: {
            jansson = prev.jansson.overrideAttrs (_: {
              # The ELF fixup expects dev/lib/cmake/jansson, but the Windows
              # package installs its targets in out/cmake and DLLs in out/bin.
              # OTPClient uses the installed pkg-config file instead.
              postFixup = "";
            });
            # libgcrypt's x86_64 MPI assembly is built with -Wa,--noexecstack,
            # which the MinGW assembler rejects. Use the C fallbacks on Windows.
            libgcrypt = prev.libgcrypt.overrideAttrs (old: {
              configureFlags = (old.configureFlags or [ ]) ++ [ "--disable-asm" ];
            });
            # libcotp passes ELF-only "-z relro/noexecstack" to the linker whenever
            # the target is not Apple; GNU ld for MinGW rejects them. Upstream
            # candidate: guard with NOT WIN32.
            libcotp = prev.libcotp.overrideAttrs (old: {
              postPatch = (old.postPatch or "") + ''
                substituteInPlace CMakeLists.txt --replace-fail "if(NOT APPLE)" "if(NOT APPLE AND NOT WIN32)"
              '';
            });
          });
        in
        {
          # Native Linux build and test: `cmake -B build && cmake --build build && ctest --test-dir build`.
          default = pkgs.mkShell {
            name = "otpclient-dev";
            nativeBuildInputs = with pkgs; [
              gcc
              clang
              clang-tools # clang-tidy, clang-format (read-only use: never reformat upstream files)
              cmake
              ninja
              pkg-config
              gettext
              glib.dev # glib-compile-resources, glib-compile-schemas
              desktop-file-utils
              gdb
              valgrind
              # tests/isolated-session.py needs these:
              python3
              dbus
              xvfb # provides Xvfb, which tests/CMakeLists.txt looks for
              xvfb-run
              bash-completion # the CLI install step probes its pkg-config file
              # misc
              git
              jq
              ripgrep
            ];
            buildInputs = libs;
            # Let the built binaries find compiled GSettings schemas and GTK data.
            shellHook = ''
              export XDG_DATA_DIRS="${pkgs.gtk4}/share/gsettings-schemas/${pkgs.gtk4.name}:${pkgs.gsettings-desktop-schemas}/share/gsettings-schemas/${pkgs.gsettings-desktop-schemas.name}:${pkgs.glib}/share/gsettings-schemas/${pkgs.glib.name}:''${XDG_DATA_DIRS:-}"
              echo "otpclient dev shell: cmake -B build -G Ninja && cmake --build build && ctest --test-dir build"
            '';
          };
        }
        // nixpkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isx86_64 {
          # Cross-compile the core to Windows (UCRT, x86_64) and run it under Wine.
          # Experimental: covers the core + CLI dependencies only. GTK4/libadwaita for
          # Windows are expected to come from MSYS2 (see docs/windows-port.md).
          windows = windowsPkgs.mkShell {
            name = "otpclient-windows-cross";
            nativeBuildInputs = with pkgs; [ cmake ninja pkg-config git ripgrep ];
            buildInputs = with windowsPkgs; [ glib libgcrypt jansson libcotp gettext libgpg-error.dev pcre2.dev ];
            packages = [ pkgs.wineWow64Packages.stable ];
            # Prefix cmake/windows.cmake searches for libintl (pass as -DOTP_WINDOWS_GETTEXT_ROOT=...).
            OTP_WINDOWS_GETTEXT_ROOT = "${windowsPkgs.gettext}";
          };
        });

      formatter = forLinux (pkgs: pkgs.nixpkgs-fmt);
    };
}
