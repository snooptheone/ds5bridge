<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Releasing

A release is a git tag `vX.Y.Z`. The [release workflow](../.github/workflows/release.yml) then builds the Arch package, the `.deb` for Ubuntu 24.04 and Ubuntu 26.04, and the Fedora
`.rpm`, publishes them with a `SHA256SUMS` file in a GitHub release, and adds the Arch package to the signed pacman repository (the fixed release `repo`).
Pull requests that touch the packaging build the same packages without publishing anything.

## One-time setup: the pacman repository key

The repository is signed with a dedicated GPG key. `tools/setup-signing-key.sh` creates it in a temporary keyring, stores the key, its passphrase and a revocation certificate in Bitwarden, and sets the
repository secrets `GPG_PRIVATE_KEY` and `GPG_PASSPHRASE` (it needs `gpg`, `gh` logged in, and `bw`; `--dry-run` stops before saving anything). Put the fingerprint it prints in the README's Arch section.
Without the secrets the workflow still publishes the packages and only skips the repository (`::notice::GPG_PRIVATE_KEY is not set`). The key expires after two years: renew it before.

## Each release

1. Change the version in **four** places: `VERSION` in the `Makefile`, `pkgver` (and `pkgrel=1`) in `packaging/arch/PKGBUILD`, the first entry of `debian/changelog`, and `Version:` plus the top `%changelog` entry of `packaging/rpm/ds5bridge.spec`.
2. Regenerate `packaging/arch/.SRCINFO`: `cd packaging/arch && env -u DS5_SRC -u DS5_REF makepkg --printsrcinfo > .SRCINFO`.
3. `make test` (it includes `tests/test_packaging.sh`, which fails if the versions disagree, the installed tree differs between packagers, or a source file has no license header).
4. Commit, then tag and push: `git tag vX.Y.Z && git push origin main vX.Y.Z`. The workflow checks that the tag equals `v` + the version before building.

A packaging-only fix of an Arch package that has been released: raise `pkgrel` in the PKGBUILD and regenerate `.SRCINFO`; the same tag cannot be released twice, so such a fix needs a new version.

## Testing the packages before a release

The packages were built and installed in clean containers (`archlinux:base-devel`, `ubuntu:24.04`, `ubuntu:26.04`, `fedora:latest`) with `make test` run inside each build. The Arch PKGBUILD
clones its source: set `DS5_SRC=file:///path/to/clone DS5_REF=commit=<sha>` to build a commit that is not tagged yet. `act` can run the workflow locally (`act pull_request -W .github/workflows/release.yml`).
