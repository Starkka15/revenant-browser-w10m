// UWP has no getenv (env vars aren't a UWP concept). BoringSSL's X509 by_file/by_dir use it
// to read SSL_CERT_FILE/SSL_CERT_DIR — meaningless on UWP (we drive CA via CURLOPT). Stub to null.
#pragma once
#include <stdlib.h>
#ifndef BSSL_UWP_GETENV_STUB
#define BSSL_UWP_GETENV_STUB
static inline char* getenv(const char* name) { (void)name; return 0; }
#endif
