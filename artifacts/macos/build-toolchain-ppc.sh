#!/bin/sh
# shellcheck disable=2086
set -e

# cd to the directory this script is in
[ "${0%/*}" = "$0" ] && scriptroot="." || scriptroot="${0%/*}"
cd "$scriptroot"

platformdir=$PWD

workdir="$PWD/build"
sdk="$workdir/ppc-sdk"
mkdir -p "$sdk"
cd "$workdir"

# Increase this if we ever make a change to the SDK, for example
# using a newer SDK version, and we need to invalidate the cache.
sdkver=1
if ! [ -d "$sdk" ] || [ "$(cat "$sdk/sdkver" 2>/dev/null)" != "$sdkver" ]; then
    printf '\nDownloading macOS SDK...\n\n'
    (
    # for old stuff
    [ -d "$sdk" ] && rm -rf "$sdk" &
    rm -f MacOSX10.5.sdk.tar.xz
    wget -q https://github.com/phracker/MacOSX-SDKs/releases/download/11.3/MacOSX10.5.sdk.tar.xz
    wait
    tar -xJf MacOSX10.5.sdk.tar.xz
    mv MacOSX10.5.sdk "$sdk"
    )
    wait
    rm ./*.tar.xz
    printf '%s' "$sdkver" > "$sdk/sdkver"
    outdated_sdk=1
fi

if command -v nproc >/dev/null; then
    ncpus="$(nproc)"
else
    ncpus="$(sysctl -n hw.ncpu)"
fi

for dep in clang make cmp; do
    if ! command -v "$dep" >/dev/null; then
        printf '%s not found!\n' "$dep"
        exit 1
    fi
done

printf '%s' "$workdir" > workdir
if ! cmp -s workdir lastworkdir; then
    rm -rf toolchain-ppc
fi
mv workdir lastworkdir

# Increase this if we ever make a change to the toolchain, for example
# using a newer cctools-port version, and we need to invalidate the cache.
toolchainver=3
if [ "$(cat toolchain-ppc/toolchainver 2>/dev/null)" != "$toolchainver" ]; then
    rm -rf toolchain-ppc
    outdated_toolchain=1
fi

mkdir -p toolchain-ppc/bin
export PATH="$PWD/toolchain-ppc/bin:$PATH"

# Increase this if we ever make a change to the toolchain, for example
# using a newer GCC version, and we need to invalidate the cache.
ppctoolchainver=1
ppc_triple='powerpc-apple-darwin8'
if [ "$(cat toolchain-ppc/toolchainver 2>/dev/null)" != "$ppctoolchainver" ]; then
    printf '\nBuilding powerpc toolchain...\n\n'

    rm -rf toolchain-ppc
    mkdir -p toolchain-ppc/bin

    # building the real dsymutil would require a partial LLVM build, we don't need debug info that bad
    printf '#!/bin/sh\nexit 0\n' > "toolchain-ppc/bin/$ppc_triple-dsymutil"
    chmod +x "toolchain-ppc/bin/$ppc_triple-dsymutil"

    cctools_commit=a35aa0162cb2614e68db577a28fdd903fae47f20
    rm -rf cctools-port-*
    wget -O- "https://github.com/Un1q32/cctools-port/archive/$cctools_commit.tar.gz" | tar -xz

    cd "cctools-port-$cctools_commit/cctools"
    ./configure \
        --target=ppc \
        --enable-silent-rules \
        --with-llvm-config=false
    make -C ld64 -j"$ncpus"
    strip ld64/src/ld/ld
    mv ld64/src/ld/ld ../../toolchain-ppc/bin/ppc-ld
    make -C libstuff -j"$ncpus"
    make -C misc nm strip ranlib lipo -j"$ncpus"
    strip misc/nm misc/strip misc/ranlib
    mv misc/nm ../../toolchain-ppc/bin/ppc-nm
    mv misc/strip ../../toolchain-ppc/bin/ppc-strip
    mv misc/ranlib ../../toolchain-ppc/bin/ppc-ranlib
    mv misc/lipo ../../toolchain-ppc/bin
    make -C as/ppc -j"$ncpus"
    strip as/ppc/ppc-as
    mv as/ppc/ppc-as ../../toolchain-ppc/bin/ppc-as
    make -C ar
    strip ar/ar
    mv ar/ar ../../toolchain-ppc/bin/ppc-ar
    cd ../..
    rm -rf "cctools-port-$cctools_commit" &

    gcc_version='16.1.0'
    rm -rf gcc-*
    wget -O- "https://ftp.gnu.org/gnu/gcc/gcc-$gcc_version/gcc-$gcc_version.tar.xz" | tar -xJ

    cd "gcc-$gcc_version"
    mkdir build
    cd build
    set --
    [ -n "$GMP" ] && set -- --with-gmp="$GMP"
    [ -n "$MPFR" ] && set -- "$@" --with-mpfr="$MPFR"
    [ -n "$MPC" ] && set -- "$@" --with-mpc="$MPC"
    ../configure \
        --prefix="$workdir/toolchain-ppc" \
        --target="$ppc_triple" \
        --disable-multilib \
        --disable-nls \
        --with-system-zlib \
        --enable-languages=c \
        --with-sysroot="$sdk" \
        --with-as="$(command -v ppc-as)" \
        --with-ld="$(command -v ppc-ld)" \
        AR_FOR_TARGET="$(command -v ppc-ar)" \
        RANLIB_FOR_TARGET="$(command -v ppc-ranlib)" \
        NM_FOR_TARGET="$(command -v ppc-nm)" \
        LIPO_FOR_TARGET="$(command -v lipo)" \
        STRIP_FOR_TARGET="$(command -v ppc-strip)" \
        "$@"
    make -j"$ncpus"
    make -j"$ncpus" install-strip
    cd ../..
    rm -rf "gcc-$gcc_version" &

    rm -rf toolchain-ppc/share
    printf '%s' "$ppctoolchainver" > toolchain-ppc/toolchainver
    wait
else
    printf 'Toolchain already built! :)\n'
fi
