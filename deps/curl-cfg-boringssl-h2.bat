@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
REM Configure curl for ARM32 UWP against BoringSSL, WITH HTTP/2 (nghttp2).
REM
REM WHY THIS EXISTS: curl-cfg-boringssl.bat (Jul 12) was derived from curl-cfg.bat (Jun 26), which
REM predated HTTP/2 and passed -DUSE_NGHTTP2=OFF. The script that actually produced the shipping lib
REM was curl-build.bat (Jul 4), which added USE_NGHTTP2=ON plus the include/library paths and the
REM /DNGHTTP2_STATICLIB define required for static linking. Branching from the wrong predecessor
REM silently dropped HTTP/2 from the BoringSSL build: build-armuwp/lib/curl_config.h has
REM "#define USE_NGHTTP2 1" (188 nghttp2 symbols in the lib) while build-armuwp-bssl2 has
REM "/* #undef USE_NGHTTP2 */" (zero). Every request in the shipped browser is HTTP/1.1, so a
REM thumbnail-heavy page opens one TCP+TLS connection per parallel resource instead of multiplexing.
REM
REM FRESH BUILD DIR (bssl3): build-armuwp-bssl2 has USE_NGHTTP2:BOOL=OFF in its CMakeCache. Re-running
REM configure over a stale cache is how this class of setting gets silently kept; a new dir cannot
REM inherit it.
REM
REM nghttp2 itself is protocol-only and does not link against any TLS library, so BoringSSL + nghttp2
REM has no conflict -- curl uses the standard ALPN APIs, which BoringSSL implements.
set CM=%CMAKE%
set INST=%ROOT%\deps\_install
"%CM%" -S %ROOT%\deps\curl -B %ROOT%\deps\curl\build-armuwp-bssl3 ^
  -G "%VSGEN%" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_DISABLE_INSTALL=ON ^
  -DCURL_USE_OPENSSL=ON -DCURL_USE_SCHANNEL=OFF ^
  -DOPENSSL_ROOT_DIR=%ROOT%\boringssl-new-build\build\Release -DOPENSSL_INCLUDE_DIR=%ROOT%\boringssl-new\include ^
  -DOPENSSL_SSL_LIBRARY=%ROOT%\boringssl-new-build\build\Release\ssl.lib -DOPENSSL_CRYPTO_LIBRARY=%ROOT%\boringssl-new-build\build\Release\crypto.lib ^
  -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR=%INST%\include -DZLIB_LIBRARY=%INST%\lib\zlib.lib ^
  -DCURL_BROTLI=ON -DBROTLI_INCLUDE_DIR=%INST%\include -DBROTLIDEC_LIBRARY=%INST%\lib\brotlidec.lib -DBROTLICOMMON_LIBRARY=%INST%\lib\brotlicommon.lib ^
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON ^
  -DUSE_NGHTTP2=ON -DNGHTTP2_INCLUDE_DIR=%INST%\include -DNGHTTP2_LIBRARY=%INST%\lib\nghttp2.lib -DNGHTTP2_LIBRARIES=%INST%\lib\nghttp2.lib ^
  -DCMAKE_C_FLAGS="/DNGHTTP2_STATICLIB" -DCMAKE_CXX_FLAGS="/DNGHTTP2_STATICLIB" ^
  -DHAVE_SSL_SET0_WBIO=1 -DHAVE_OPENSSL_SRP=0 -DCURL_DISABLE_SRP=ON -DHAVE_SSL_CTX_SET_QUIC_METHOD=1 -DHAVE_ECH=0 > %ROOT%\curl-h2-cfg.log 2>&1
echo CURL_CFG=%ERRORLEVEL%>> %ROOT%\curl-h2-cfg.log
REM Prove the setting actually took, rather than trusting the flag: curl_config.h is the ground truth.
findstr /C:"USE_NGHTTP2" %ROOT%\deps\curl\build-armuwp-bssl3\lib\curl_config.h >> %ROOT%\curl-h2-cfg.log 2>&1
type %ROOT%\curl-h2-cfg.log
