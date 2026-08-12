/*
 * tls_iconv_stubs.c — graceful-failure stubs so the player links without
 * OpenSSL and libiconv (LITE build). ffmpeg's tls_openssl.o is pulled by
 * the protocol registry; with these stubs any https:// open fails at
 * runtime with a clean error instead of dragging in ~9 MB of TLS code.
 * http:// (DLNA, most IPTV) is completely unaffected.
 */

/* ---- OpenSSL stubs: every call fails ---- */
void *__attribute__((weak)) BIO_meth_new(int type, const char *name) { (void)type; (void)name; return 0; }
void __attribute__((weak)) BIO_meth_free(void *m) { (void)m; }
int __attribute__((weak)) BIO_meth_set_write(void *m, void *f) { (void)m; (void)f; return 0; }
int __attribute__((weak)) BIO_meth_set_read(void *m, void *f) { (void)m; (void)f; return 0; }
int __attribute__((weak)) BIO_meth_set_puts(void *m, void *f) { (void)m; (void)f; return 0; }
int __attribute__((weak)) BIO_meth_set_create(void *m, void *f) { (void)m; (void)f; return 0; }
int __attribute__((weak)) BIO_meth_set_destroy(void *m, void *f) { (void)m; (void)f; return 0; }
int __attribute__((weak)) BIO_meth_set_ctrl(void *m, void *f) { (void)m; (void)f; return 0; }
void *__attribute__((weak)) BIO_new(void *m) { (void)m; return 0; }
void __attribute__((weak)) BIO_set_data(void *b, void *p) { (void)b; (void)p; }
void *__attribute__((weak)) BIO_get_data(void *b) { (void)b; return 0; }
void __attribute__((weak)) BIO_set_flags(void *b, int f) { (void)b; (void)f; }
void __attribute__((weak)) BIO_clear_flags(void *b, int f) { (void)b; (void)f; }
void __attribute__((weak)) BIO_set_init(void *b, int i) { (void)b; (void)i; }
void *__attribute__((weak)) TLS_client_method(void) { return 0; }
void *__attribute__((weak)) TLS_server_method(void) { return 0; }
void *__attribute__((weak)) SSL_CTX_new(void *m) { (void)m; return 0; }
void __attribute__((weak)) SSL_CTX_free(void *c) { (void)c; }
long __attribute__((weak)) SSL_CTX_set_options(void *c, long o) { (void)c; (void)o; return 0; }
void __attribute__((weak)) SSL_CTX_set_verify(void *c, int mode, void *cb) { (void)c; (void)mode; (void)cb; }
int __attribute__((weak)) SSL_CTX_load_verify_locations(void *c, const char *a, const char *b) { (void)c; (void)a; (void)b; return 0; }
int __attribute__((weak)) SSL_CTX_use_certificate_chain_file(void *c, const char *f, int t) { (void)c; (void)f; (void)t; return 0; }
int __attribute__((weak)) SSL_CTX_use_PrivateKey_file(void *c, const char *f, int t) { (void)c; (void)f; (void)t; return 0; }
void *__attribute__((weak)) SSL_new(void *c) { (void)c; return 0; }
void __attribute__((weak)) SSL_free(void *s) { (void)s; }
int __attribute__((weak)) SSL_connect(void *s) { (void)s; return -1; }
int __attribute__((weak)) SSL_accept(void *s) { (void)s; return -1; }
int __attribute__((weak)) SSL_read(void *s, void *b, int n) { (void)s; (void)b; (void)n; return -1; }
int __attribute__((weak)) SSL_write(void *s, const void *b, int n) { (void)s; (void)b; (void)n; return -1; }
int __attribute__((weak)) SSL_shutdown(void *s) { (void)s; return 0; }
int __attribute__((weak)) SSL_get_error(const void *s, int r) { (void)s; (void)r; return 1; }
long __attribute__((weak)) SSL_ctrl(void *s, int cmd, long larg, void *parg) { (void)s; (void)cmd; (void)larg; (void)parg; return 0; }
int __attribute__((weak)) SSL_set_bio(void *s, void *r, void *w) { (void)s; (void)r; (void)w; return 0; }

/* ---- libiconv stubs: conversion "not available", ffmpeg falls back ---- */
void *__attribute__((weak)) libiconv_open(const char *to, const char *from) { (void)to; (void)from; return (void *)-1; }
unsigned long __attribute__((weak)) libiconv(void *cd, char **in, unsigned long *inleft,
                       char **out, unsigned long *outleft) {
    (void)cd; (void)in; (void)inleft; (void)out; (void)outleft;
    return (unsigned long)-1;
}
int __attribute__((weak)) libiconv_close(void *cd) { (void)cd; return 0; }
