#!/usr/bin/env bash
# Tag a firmware release: CI builds it and publishes a GitHub Release, which
# spectre-server then serves to the app's install screen.
#
#   scripts/release.sh v2.1.0
#
# Checks that both repos are committed and pushed, records the spectre_protocol
# commit the firmware was built with (spectre_protocol.ref, read by CI), then
# tags and pushes.
set -euo pipefail
cd "$(dirname "$0")/.."
PROTO=../../spectre_protocol

version=${1:-}
[[ $version =~ ^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$ ]] || { echo "usage: $0 vMAJOR.MINOR.PATCH[-rc1]" >&2; exit 1; }
git rev-parse -q --verify "refs/tags/$version" >/dev/null && { echo "$version already exists" >&2; exit 1; }

clean() { [[ -z $(git -C "$1" status --porcelain --untracked-files=no) ]] || { echo "$1: uncommitted changes" >&2; exit 1; }; }
pushed() {  # HEAD is on its remote branch
  git -C "$1" fetch -q
  git -C "$1" branch -r --contains HEAD | grep -q . || { echo "$1: push it first" >&2; exit 1; }
}
clean . && clean "$PROTO"
pushed "$PROTO"

ref=$(git -C "$PROTO" rev-parse HEAD)
if [[ $(cat spectre_protocol.ref 2>/dev/null) != "$ref" ]]; then
  echo "$ref" > spectre_protocol.ref
  git commit -q -m "spectre_protocol $(git -C "$PROTO" rev-parse --short HEAD) for $version" spectre_protocol.ref
fi
git tag -a "$version" -m "Firmware $version"
git push -q origin HEAD "$version"
echo "$version pushed: CI builds it, then https://github.com/spectrenoir06/matrix_ble_player/releases"
