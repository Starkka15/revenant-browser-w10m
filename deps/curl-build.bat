@echo off
set CM=Z:\w10m-webengine\tools\cmake\bin\cmake.exe
set I=Z:/w10m-webengine/deps/_install
"%CM%" -S Z:\w10m-webengine\deps\curl -B Z:\w10m-webengine\deps\curl\build-armuwp ^
  -G "Visual Studio 17 2022" -A ARM -T v142,host=x64 ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.16299.0 ^
  -DCMAKE_TOOLCHAIN_FILE=Z:\w10m-webengine\cmake\Toolchain-W10M-ARM32-UWP.cmake ^
  -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DCURL_DISABLE_INSTALL=ON ^
  -DCURL_USE_OPENSSL=ON -DCURL_USE_SCHANNEL=OFF ^
  -DOPENSSL_ROOT_DIR=%I% -DOPENSSL_INCLUDE_DIR=%I%/include ^
  -DOPENSSL_SSL_LIBRARY=%I%/lib/libssl.lib -DOPENSSL_CRYPTO_LIBRARY=%I%/lib/libcrypto.lib ^
  -DCURL_ZLIB=ON -DZLIB_INCLUDE_DIR=%I%/include -DZLIB_LIBRARY=%I%/lib/zlib.lib ^
  -DCURL_BROTLI=ON -DBROTLI_INCLUDE_DIR=%I%/include -DBROTLIDEC_LIBRARY=%I%/lib/brotlidec.lib -DBROTLICOMMON_LIBRARY=%I%/lib/brotlicommon.lib ^
  -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON ^
  -DUSE_NGHTTP2=ON -DNGHTTP2_INCLUDE_DIR=%I%/include -DNGHTTP2_LIBRARY=%I%/lib/nghttp2.lib -DNGHTTP2_LIBRARIES=%I%/lib/nghttp2.lib ^
  -DCMAKE_C_FLAGS="/DNGHTTP2_STATICLIB" -DCMAKE_CXX_FLAGS="/DNGHTTP2_STATICLIB"
echo CURL_CFG=%ERRORLEVEL%
"%CM%" --build Z:\w10m-webengine\deps\curl\build-armuwp --config Release --target libcurl_static
echo CURL_BUILD=%ERRORLEVEL%
copy /Y Z:\w10m-webengine\deps\curl\build-armuwp\lib\Release\libcurl.lib %I%\lib\libcurl.lib
echo CURL_STAGE=%ERRORLEVEL%
echo CURL_BUILD=%ERRORLEVEL%
