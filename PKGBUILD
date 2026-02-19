# Maintainer: anthonyhab
pkgname=bb-auth-git
pkgver=r43.fc40397
pkgrel=1
pkgdesc="Unified polkit, keyring, and pinentry authentication daemon"
arch=('x86_64')
url="https://github.com/anthonyhab/bb-auth"
license=('BSD-3-Clause')
depends=(
    'qt6-base'
    'polkit-qt6'
    'polkit'
    'gnome-keyring'
    'json-glib'
)
makedepends=(
    'git'
    'cmake'
)
optdepends=(
    'fprintd: Fingerprint authentication support'
    'libfido2: FIDO2 device support'
)
provides=('bb-auth')
conflicts=('bb-auth')
replaces=('noctalia-auth-git' 'noctalia-polkit-git' 'noctalia-unofficial-auth-agent-git')
source=("${pkgname}::git+https://github.com/anthonyhab/bb-auth.git")
sha256sums=('SKIP')

pkgver() {
    cd "$pkgname"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cd "$pkgname"
    local jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-${JOBS:-$(nproc)}}"
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build -j"$jobs"
}

check() {
    cd "$pkgname"
    local jobs="${CTEST_PARALLEL_LEVEL:-${JOBS:-$(nproc)}}"
    ctest --test-dir build --output-on-failure --parallel "$jobs"
}

package() {
    cd "$pkgname"
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
