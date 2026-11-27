#!/bin/sh
# Build both architectures and publish one GitHub release containing the
# update packages, plus patches from the last few releases that can apply them.
# Needs the signing key and an authenticated gh.
#   tools/release.sh                 uses v<version>.<build number> as the tag
#   tools/release.sh --dry-run       builds the packages and stops before publishing
set -e
cd "$(dirname "$0")/.."
REPO=Banaxi-Tech/KleinOS
FIRST_PATCH_VERSION=1.0.191   # the first version whose updater understands patches
PATCH_RELEASES=5

dry_run=""
if [ "$1" = "--dry-run" ]; then dry_run=1; shift; fi

# main.sub.build as one comparable number
version_key() {
    IFS=. read -r main sub build <<V
${1#v}
V
    echo $((main * 100000000 + sub * 1000000 + build))
}

ver=$(sed -n 's/^#define KLEIN_VERSION "\([0-9.]*\)".*/\1/p' kernel/sys/system.h)
tag=${1:-v$ver.$(cat BUILD_NUMBER)}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
first_key=$(version_key "$FIRST_PATCH_VERSION")
this_key=$(version_key "$tag")
tags=$(gh release list --repo "$REPO" --limit "$PATCH_RELEASES" --json tagName --jq '.[].tagName')
rm -rf build32/release build/release
for arch in i386 x86_64; do
    dir=build
    [ "$arch" = i386 ] && dir=build32
    # Both packages must carry the same build number and be marked as releases.
    # DEV_VERSION= keeps the build number still; the stamp makes it rewrite the header.
    rm -f "$dir/buildnum.stamp"
    make ARCH="$arch" DEV_VERSION= -j"$(nproc)" "$dir/kernel.elf" "$dir/initrd.tar"
    from=""
    for t in $tags; do
        key=$(version_key "$t")
        [ "$key" -ge "$first_key" ] && [ "$key" -lt "$this_key" ] || continue
        mkdir -p "$work/$arch/$t"
        gh release download "$t" --repo "$REPO" --pattern "KleinOS-*-$arch.kupd" --dir "$work/$arch/$t" || continue
        for f in "$work/$arch/$t"/*.kupd; do
            case "$f" in *-from-*) ;; *) from="$from $f" ;; esac
        done
    done
    make ARCH="$arch" DEV_VERSION= kupd TAG="$tag" PATCH_FROM="$from"
done
if [ -n "$dry_run" ]; then
    echo "dry run: packages for $tag are in build32/release and build/release, nothing was published"
    exit 0
fi
gh release create "$tag" --repo "$REPO" --title "KleinOS $tag" --generate-notes build32/release/* build/release/*
