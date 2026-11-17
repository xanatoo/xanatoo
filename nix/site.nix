# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#
# The website (https://xanatoo.github.io, sources in website/) and the
# downloads it serves.
#
#   nix build .#release-<target>   one target's release assets + <target>.json
#                                  (android, r36s, win64, win32, wasm)
#   nix build .#site               the whole site, every build from this tree
#   nix run .#site-serve [DIR]     serve DIR (default: .#site) on 127.0.0.1
#   nix run .#site-from-release -- OUT
#                                  the deployed site: downloads from the
#                                  `continuous` release (else the live site)
#   nix run .#publish-build -- TAG TARGET DIR
#                                  CI: put DIR (= .#release-TARGET) on TAG
#
# CI (artifacts.yml) builds each release-<target>, publishes it and asks
# xanatoo.github.io to run site-from-release; see docs/dev/PLATFORM.md.
{ pkgs, self, version, packages }:
let
  lib = pkgs.lib;
  website = lib.cleanSource ../website;
  lastModified = self.lastModified or 1;

  metaJson = target: file: pkgs.writeText "${target}.json" (builtins.toJSON {
    inherit target version lastModified file;
    rev = self.rev or self.dirtyRev or "";
    revCount = self.revCount or 0;
  });

  # Asset names carry the version and the platform, so downloads of
  # different builds never collide (`+` is not safe in GitHub asset names).
  # <target>.json names the file; fetch-builds.sh and assemble.sh read it.
  fileVersion = lib.replaceStrings [ "+" ] [ "-" ] version;
  files = {
    android = "xanatoo-${fileVersion}-android-universal.apk";
    # PortMaster installs the zip under the name its port.json gives.
    r36s = "xanatoo.zip";
    win64 = "xanatoo-${fileVersion}-windows-x86_64.zip";
    win32 = "xanatoo-${fileVersion}-windows-i686.zip";
    wasm = "xanatoo-${fileVersion}-wasm32.tar.gz";
  };

  release = target: install: pkgs.runCommand "xanatoo-release-${target}" {
    nativeBuildInputs = [ pkgs.gnutar pkgs.gzip ];
  } ''
    mkdir $out
    file=$out/${files.${target}}
    ${install}
    cp ${metaJson target files.${target}} $out/${target}.json
  '';

  releases = {
    android = release "android" ''
      cp ${packages.xanatoo-android}/*.apk "$file"
    '';
    r36s = release "r36s" ''
      cp ${packages.xanatoo-r36s-portmaster-zip}/xanatoo.zip "$file"
    '';
    win64 = release "win64" ''
      cp ${packages.xanatoo-win64-zip}/*.zip "$file"
    '';
    win32 = release "win32" ''
      cp ${packages.xanatoo-win32-zip}/*.zip "$file"
    '';
    # The browser edition is a directory; the release keeps it as one file.
    wasm = release "wasm" ''
      tar -C ${packages.xanatoo-wasm} --sort=name --mtime=@${toString lastModified} \
        --owner=0 --group=0 --numeric-owner -ch . | gzip -9n > "$file"
    '';
  };

  # The layout fetch-builds.sh produces, from the release assets above.
  siteBuilds = pkgs.runCommand "xanatoo-site-builds" {
    nativeBuildInputs = [ pkgs.gnutar pkgs.gzip ];
  } ''
    mkdir -p $out/versions $out/wasm
    ${lib.concatMapStrings (t: ''
      cp ${releases.${t}}/${t}.json $out/versions/
    '') (lib.attrNames releases)}
    ${lib.concatMapStrings (t: ''
      cp ${releases.${t}}/${files.${t}} $out/
    '') [ "android" "r36s" "win64" "win32" ]}
    tar -C $out/wasm -xzf ${releases.wasm}/${files.wasm}
  '';

  site = pkgs.runCommand "xanatoo-site-${version}" {
    nativeBuildInputs = [ pkgs.jq ];
  } ''
    bash ${website}/assemble.sh ${siteBuilds} $out
  '';

  app = name: runtimeInputs: text: description: {
    type = "app";
    program = lib.getExe (pkgs.writeShellApplication { inherit name runtimeInputs text; });
    meta.description = description;
  };
in {
  packages = { inherit site; site-builds = siteBuilds; }
    // lib.mapAttrs' (t: drv: lib.nameValuePair "release-${t}" drv) releases;

  apps = {
    site-from-release = app "xanatoo-site-from-release"
      [ pkgs.curl pkgs.jq pkgs.gnutar pkgs.gzip pkgs.coreutils ] ''
        out=''${1:?usage: site-from-release OUT}
        builds=$(mktemp -d)
        trap 'rm -rf "$builds"' EXIT
        bash ${website}/fetch-builds.sh "$builds"
        bash ${website}/assemble.sh "$builds" "$out"
      '' "Assemble the website from the latest release builds";

    site-serve = app "xanatoo-site-serve" [ pkgs.python3 ] ''
      dir=''${1:-${site}}
      port=''${XANATOO_SITE_PORT:-8765}
      echo "Serving $dir at http://127.0.0.1:$port/  (Ctrl-C to stop)"
      exec python3 -m http.server "$port" --bind 127.0.0.1 --directory "$dir"
    '' "Serve the website locally";

    publish-build = app "xanatoo-publish-build"
      [ pkgs.gh pkgs.curl pkgs.jq pkgs.gawk pkgs.coreutils ] ''
        exec bash ${website}/publish-build.sh "$@"
      '' "Replace one target's assets on a GitHub release (CI)";
  };
}
