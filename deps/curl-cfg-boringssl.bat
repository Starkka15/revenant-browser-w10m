@echo off
if not defined ROOT call "%~dp0..\env.bat"
if not defined ROOT ( echo [!] env.bat not found - copy env.example.bat to env.bat and edit it & exit /b 1 )
set CM=%CMAKE%
set INST=%ROOT%\deps\_install
"%CM%" -S %ROOT%\deps\curl -B %ROOT%\deps\curl\build-armuwp-bssl2 ^
  -G "%VSGEN%" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=%ROOT%\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_DISABLE_INSTALL=ON ^
  -DCURL_USE_OPENSSL=ON -DCURL_USE_SCHANNEL=OFF ^
  -DOPENSSL_ROOT_DIR=%ROOT%\boringssl-new-build\build\Release -DOPENSSL_INCLUDE_DIR=%ROOT%\boringssl-new\include ^
  -DOPENSSL_SSL_LIBRARY=%ROOT%\boringssl-new-build\build\Release\ssl.lib -DOPENSSL_CRYPTO_LIBRARY=%ROOT%\boringssl-new-build\build\Release\crypto.lib ^
  -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR=%INST%\include -DZLIB_LIBRARY=%INST%\lib\zlib.lib ^
  -DCURL_BROTLI=ON -DBROTLI_INCLUDE_DIR=%INST%\include -DBROTLIDEC_LIBRARY=%INST%\lib\brotlidec.lib -DBROTLICOMMON_LIBRARY=%INST%\lib\brotlicommon.lib ^
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DUSE_NGHTTP2=OFF ^
  -DHAVE_SSL_SET0_WBIO=1 -DHAVE_OPENSSL_SRP=0 -DCURL_DISABLE_SRP=ON -DHAVE_SSL_CTX_SET_QUIC_METHOD=1 -DHAVE_ECH=0 > %ROOT%\curl-bssl-cfg.log 2>&1
echo CURL_CFG=%ERRORLEVEL%>> %ROOT%\curl-bssl-cfg.log
