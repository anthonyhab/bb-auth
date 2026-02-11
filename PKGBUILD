# Maintainer: Anthony Habibe <anthony@noctalia.dev>
pkgname=noctalia-auth-git
pkgver=r43.fc40397
pkgrel=1
pkgdesc="Unified polkit, keyring, and pinentry authentication daemon for noctalia-shell"
arch=('x86_64')
url="https://github.com/anthonyhab/noctalia-polkit"
license=('BSD-3-Clause')
depends=(
    'qt6-base'
    'polkit-qt6'
    'polkit'
    'gcr-4'
    'json-glib'
)
makedepends=(
    'git'
    'cmake'
)
provides=('noctalia-auth' 'noctalia-auth-git')
conflicts=('noctalia-auth' 'noctalia-polkit-git' 'noctalia-unofficial-auth-agent')
replaces=('noctalia-polkit-git')
source=("${pkgname}::git+https://github.com/anthonyhab/noctalia-polkit.git")
sha256sums=('SKIP')

pkgver() {
    cd "$pkgname"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cd "$pkgname"
    cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build
}

package() {
    cd "$pkgname"
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
