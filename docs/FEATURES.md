# Feature map

This file names each feature and the check for that feature.

## Packaging and distribution

hotmon ships as a pacman package, an AUR package, a Debian/Ubuntu `.deb`, and a Fedora `.rpm`. GitHub CI builds and tests each one, and a version tag publishes them as a GitHub release.

- Entry point: `scripts/packaging/build-deb.sh`, `build-rpm.sh`, `build-pacman.sh`; workflows `.github/workflows/packages.yml` and `release.yml`.
- Use:
  1. Make the source tarball: `scripts/packaging/make-source-tarball.sh 0.1.0 dist`.
  2. Build one package: `scripts/packaging/build-deb.sh 0.1.0 dist/hotmon-0.1.0.tar.gz dist` (in a Debian container; `build-rpm.sh` in Fedora; `build-pacman.sh` in Arch).
  3. Install it: `sudo apt install ./dist/hotmon_0.1.0-1_amd64.deb`.
  4. Release: change the version in `CMakeLists.txt`, `packaging/aur/PKGBUILD`, `packaging/rpm/hotmon.spec`, `packaging/debian/changelog`; then `git tag v<version> && git push origin v<version>`.
- Check: `ctest --test-dir build -R '^Packaging\.' --output-on-failure` prints `100% tests passed`. After install, `hotmon x; echo $?` prints `Usage: hotmon` and `2`.
- Code and tests: `CMakeLists.txt`, `packaging/`, `scripts/packaging/`, `.github/workflows/packages.yml`, `.github/workflows/release.yml`, `tests/packaging/`.
