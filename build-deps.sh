#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
THIRD_PARTY="$ROOT_DIR/third_party"

mkdir -p "$THIRD_PARTY"

# Detect OS
OS="$(uname -s)"
echo "Detected OS: $OS"

# Optional: set to "universal" for macOS fat binaries (arm64+x86_64)
TARGET_ARCH="${TARGET_ARCH:-}"

echo "Using cmake from: $(which cmake)"
cmake --version

# -----------------------------
# FFmpeg
# -----------------------------

FFMPEG="ffmpeg-8.0"
FFMPEG_DIR="$THIRD_PARTY/ffmpeg"
FFMPEG_BUILD="$FFMPEG_DIR/build"

if [ -d "$FFMPEG_BUILD" ]; then
    echo "FFmpeg already built at $FFMPEG_BUILD"
else
    echo "Building FFmpeg..."
    mkdir -p "$FFMPEG_DIR"
    cd "$FFMPEG_DIR"

    if [ ! -d "$FFMPEG" ]; then
        curl -L -O "https://ffmpeg.org/releases/$FFMPEG.tar.xz"
        tar xf "$FFMPEG.tar.xz"
    fi

    # Common FFmpeg configure flags
    FFMPEG_COMMON_OPTS="--disable-all --disable-autodetect \
        --disable-static --enable-shared \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swresample \
        --enable-decoder=adpcm_adx --enable-parser=adx --enable-muxer=adx"

    case "$OS" in
        Darwin)
            if [ "$TARGET_ARCH" = "universal" ]; then
                echo "Building FFmpeg universal binary (arm64 + x86_64)..."

                # Build arm64
                cd "$FFMPEG_DIR/$FFMPEG"
                mkdir -p build-arm64 && cd build-arm64
                ../configure \
                    --prefix="$FFMPEG_DIR/build-arm64-out" \
                    $FFMPEG_COMMON_OPTS \
                    --enable-pic --extra-cflags="-fPIC -arch arm64" \
                    --extra-ldflags="-arch arm64 -Wl,-rpath,@loader_path/../Frameworks" \
                    --install-name-dir="@rpath" \
                    --arch=arm64
                make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
                make install

                # Build x86_64
                cd "$FFMPEG_DIR/$FFMPEG"
                mkdir -p build-x86_64 && cd build-x86_64
                ../configure \
                    --prefix="$FFMPEG_DIR/build-x86_64-out" \
                    $FFMPEG_COMMON_OPTS \
                    --enable-pic --extra-cflags="-fPIC -arch x86_64" \
                    --extra-ldflags="-arch x86_64 -Wl,-rpath,@loader_path/../Frameworks" \
                    --install-name-dir="@rpath" \
                    --arch=x86_64 --enable-cross-compile --target-os=darwin
                make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
                make install

                # Merge into universal fat binaries via lipo
                mkdir -p "$FFMPEG_BUILD/lib" "$FFMPEG_BUILD/include"
                cp -R "$FFMPEG_DIR/build-arm64-out/include/"* "$FFMPEG_BUILD/include/"
                for dylib in "$FFMPEG_DIR/build-arm64-out/lib/"*.dylib; do
                    basename=$(basename "$dylib")
                    x86_dylib="$FFMPEG_DIR/build-x86_64-out/lib/$basename"
                    if [ -L "$dylib" ]; then
                        # Preserve symlinks
                        cp -P "$dylib" "$FFMPEG_BUILD/lib/"
                    elif [ -f "$x86_dylib" ]; then
                        lipo -create "$dylib" "$x86_dylib" -output "$FFMPEG_BUILD/lib/$basename"
                        echo "  lipo: $basename"
                    else
                        cp "$dylib" "$FFMPEG_BUILD/lib/"
                    fi
                done

                # Clean up per-arch builds
                rm -rf "$FFMPEG_DIR/build-arm64-out" "$FFMPEG_DIR/build-x86_64-out"
            else
                cd "$FFMPEG_DIR/$FFMPEG"
                mkdir -p build && cd build
                ../configure \
                    --prefix=$FFMPEG_BUILD \
                    $FFMPEG_COMMON_OPTS \
                    --enable-pic \
                    --extra-cflags="-fPIC" \
                    --extra-ldflags="-Wl,-rpath,@loader_path/../Frameworks" \
                    --install-name-dir="@rpath"
                make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
                make install
            fi
            ;;
        Linux)
            cd "$FFMPEG_DIR/$FFMPEG"
            mkdir -p build && cd build
            ../configure \
                --prefix=$FFMPEG_BUILD \
                $FFMPEG_COMMON_OPTS \
                --enable-pic \
                --extra-cflags="-fPIC" \
                --extra-ldflags="-Wl,-rpath,\$ORIGIN/../lib" \
                --install-name-dir=\$ORIGIN
            make -j$(nproc)
            make install
            ;;
        MINGW*|MSYS*|CYGWIN*)
            cd "$FFMPEG_DIR/$FFMPEG"
            mkdir -p build && cd build
            ../configure \
                --prefix=$FFMPEG_BUILD \
                $FFMPEG_COMMON_OPTS \
                --extra-cflags="-I/mingw64/include" \
                --extra-ldflags="-L/mingw64/lib"
            make -j$(nproc)
            make install
            ;;
        *)
            echo "Unsupported OS: $OS"
            exit 1
            ;;
    esac

    echo "FFmpeg installed to $FFMPEG_BUILD"

    cd "$FFMPEG_DIR"
    rm -rf "$FFMPEG"
    rm -f "$FFMPEG.tar.xz"
fi

# -----------------------------
# SDL3
# -----------------------------

SDL="SDL3-3.2.24"
SDL_DIR="$THIRD_PARTY/sdl3"
SDL_BUILD="$SDL_DIR/build"

if [ -d "$SDL_BUILD" ]; then
    echo "SDL3 already built at $SDL_BUILD"
else
    echo "Building SDL3..."
    mkdir -p "$SDL_DIR"
    cd "$SDL_DIR"

    if [ ! -d "$SDL" ]; then
        curl -L -O "https://libsdl.org/release/$SDL.tar.gz"
        tar xf "$SDL.tar.gz"
    fi

    cd "$SDL"

    mkdir -p build
    cd build

    case "$OS" in
        Darwin)
            SDL_CMAKE_OPTS="-DCMAKE_INSTALL_PREFIX=$SDL_BUILD -DBUILD_SHARED_LIBS=ON -DSDL_STATIC=OFF"
            if [ "$TARGET_ARCH" = "universal" ]; then
                SDL_CMAKE_OPTS="$SDL_CMAKE_OPTS -DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
            fi
            cmake .. $SDL_CMAKE_OPTS
            ;;
        Linux)
            cmake .. \
                -DCMAKE_INSTALL_PREFIX="$SDL_BUILD" \
                -DBUILD_SHARED_LIBS=ON \
                -DSDL_STATIC=OFF
            ;;
        MINGW*|MSYS*|CYGWIN*)
            cmake .. \
                -DCMAKE_INSTALL_PREFIX="$SDL_BUILD" \
                -DBUILD_SHARED_LIBS=ON
            ;;
    esac

    cmake --build . -j$(nproc)
    cmake --install .
    echo "SDL3 installed to $SDL_BUILD"

    cd ../..
    rm -rf "$SDL"
    rm "$SDL.tar.gz"
fi

echo "All dependencies installed successfully in $THIRD_PARTY"
