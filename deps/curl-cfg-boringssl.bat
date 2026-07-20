@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set INST=Z:\w10m-webengine\deps\_install
"%CM%" -S Z:\w10m-webengine\deps\curl -B Z:\w10m-webengine\deps\curl\build-armuwp-bssl2 ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_DISABLE_INSTALL=ON ^
  -DCURL_USE_OPENSSL=ON -DCURL_USE_SCHANNEL=OFF ^
  -DOPENSSL_ROOT_DIR=Z:\w10m-webengine\boringssl-new-build\build\Release -DOPENSSL_INCLUDE_DIR=Z:\w10m-webengine\boringssl-new\include ^
  -DOPENSSL_SSL_LIBRARY=Z:\w10m-webengine\boringssl-new-build\build\Release\ssl.lib -DOPENSSL_CRYPTO_LIBRARY=Z:\w10m-webengine\boringssl-new-build\build\Release\crypto.lib ^
  -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR=%INST%\include -DZLIB_LIBRARY=%INST%\lib\zlib.lib ^
  -DCURL_BROTLI=ON -DBROTLI_INCLUDE_DIR=%INST%\include -DBROTLIDEC_LIBRARY=%INST%\lib\brotlidec.lib -DBROTLICOMMON_LIBRARY=%INST%\lib\brotlicommon.lib ^
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON -DUSE_NGHTTP2=OFF ^
  -DHAVE_SSL_SET0_WBIO=1 -DHAVE_OPENSSL_SRP=0 -DCURL_DISABLE_SRP=ON -DHAVE_SSL_CTX_SET_QUIC_METHOD=1 -DHAVE_ECH=0 > Z:\w10m-webengine\curl-bssl-cfg.log 2>&1
echo CURL_CFG=%ERRORLEVEL%>> Z:\w10m-webengine\curl-bssl-cfg.log
