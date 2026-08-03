# Dependency manifest — Revenant (WebKit 2.36.8, W10M ARM32 UWP)

Each dependency is built from the pinned upstream commit below, with the port's changes
applied from `patches/deps/<name>.patch` (only those with a patch are modified; the rest are
vendored unchanged). Build order and flags: see BUILD.md and the per-dep scripts in deps/.

| Dependency | Upstream | Pinned base | Modified? (patch) |
|---|---|---|---|
| WebKit | https://github.com/WebKit/WebKit | 5ceba4a608 (tag webkitgtk-2.36.8) | patches/webkit-2.36.8-revenant.patch + src/ |
| BoringSSL | https://boringssl.googlesource.com/boringssl | 5ac7567 | patches/deps/boringssl.patch (crypto/thread_win.cc) |
| curl | https://github.com/curl/curl.git | 75a2079 | patches/deps/curl.patch (HTTP/2 fingerprint) |
| nghttp2 | https://github.com/nghttp2/nghttp2 | see deps/nghttp2-build.bat (tarball) | no |
| pixman | https://gitlab.freedesktop.org/pixman/pixman.git | 37216a3 | patches/deps/pixman.patch (NEON ARM32) |
| cairo | https://gitlab.freedesktop.org/cairo/cairo.git | 200441e | patches/deps/cairo.patch (DirectWrite/UWP) |
| harfbuzz | https://github.com/harfbuzz/harfbuzz.git | 096aaa6 | patches/deps/harfbuzz.patch (meson) |
| zlib | https://github.com/madler/zlib.git | 51b7f2a | patches/deps/zlib.patch |
| WAMR | https://github.com/bytecodealliance/wasm-micro-runtime.git | 25bd7eb | patches/deps/wamr.patch (UWP thread + version) |
| libxml2 | https://github.com/GNOME/libxml2.git | 505e2e8 | no |
| libpng | https://github.com/glennrp/libpng.git | ed217e3 | no |
| libjpeg-turbo | https://github.com/libjpeg-turbo/libjpeg-turbo.git | 7fa4b5b | no |
| libwebp | https://github.com/webmproject/libwebp.git | 845d547 | no |
| libpsl | https://github.com/rockdaboot/libpsl.git | ba4c49a | no |
| woff2 | https://github.com/google/woff2.git | 1bccf20 | no |
| brotli | https://github.com/google/brotli.git | ed738e8 | no |
| sqlite | https://github.com/sqlite/sqlite.git | b74eb00 | no |
| openssl | https://github.com/openssl/openssl.git | 9cff14fd | no (WebCrypto backend; TLS uses BoringSSL) |

ICU: prebuilt static, see deps/icu-build-static.bat. Toolchain: VS2022 v142, SDK 10.0.16299.0,
CMake in tools/. Full configure line: scripts/configure-wincairo.bat.
