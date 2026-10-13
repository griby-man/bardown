#!/usr/bin/env bash
# puts generated/ where the macOS build on the GitHub mirror can reach it: compressed, encrypted, and attached
# to a release of the private mirror, never committed. the build machine decrypts it with the GENERATED_KEY
# secret; the key itself stays in ~/.config/bardown/ci-key. run it after each codegen.
#   scripts/ci-generated.sh            (first time: makes the key and sets the secret)
set -euo pipefail
root="$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)"
repo="${BARDOWN_GITHUB:-griby-man/bardown}"
key="${XDG_CONFIG_HOME:-$HOME/.config}/bardown/ci-key"

if [[ ! -s $key ]]; then
  mkdir -p "$(dirname "$key")"
  (umask 077 && head -c 48 /dev/urandom | base64 > "$key")
  gh secret set GENERATED_KEY -R "$repo" < "$key"
  echo "new key in $key, set as GENERATED_KEY on $repo"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tar -C "$root" -cf - generated | zstd -19 -T0 -q \
  | gpg --batch --yes --symmetric --cipher-algo AES256 --passphrase-file "$key" -o "$work/generated.tar.zst.gpg"

# a release that only carries this file, kept off the release list's top as a draft-free prerelease
if ! gh release view ci-generated -R "$repo" > /dev/null 2>&1; then
  gh release create ci-generated -R "$repo" --prerelease --title "build input (private)" \
    --notes "Encrypted build input for CI. Not a release." --target main
fi
gh release upload ci-generated -R "$repo" --clobber "$work/generated.tar.zst.gpg"
echo "uploaded generated/ ($(du -h "$work/generated.tar.zst.gpg" | cut -f1)) to $repo"
