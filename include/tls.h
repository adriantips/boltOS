#pragma once
#include <stdint.h>

/* ===========================================================================
 *  TLS client over the BoltOS TCP stack. Negotiates TLS 1.3 (X25519 key share,
 *  TLS_AES_128/256_GCM_SHA256/384) and falls back to TLS 1.2 (ECDHE X25519 /
 *  P-256 + AES-GCM) when the server lacks 1.3.
 *
 *  On the 1.3 path the server's X.509 chain is validated (net/x509.c): hostname
 *  against the leaf SAN, validity dates against the RTC, the full cryptographic
 *  chain linkage (RSA PKCS#1v1.5 / PSS and ECDSA P-256/P-384), and the
 *  CertificateVerify signature. Trust anchoring is enforced when roots are
 *  installed (x509_add_root); with an empty store the anchor is left unverified
 *  rather than blocking connectivity. Blocking and poll-driven, like tcp_*.
 * ===========================================================================*/

struct tls_conn;

/* Open a TLS session to dst_ip:port. `sni` is the hostname sent in the SNI
 * extension (required by most CDNs). Returns a connection or 0 on failure. */
struct tls_conn *tls_connect(uint32_t dst_ip, uint16_t port,
                             const char *sni, uint32_t timeout_ms);

int  tls_send(struct tls_conn *c, const void *data, uint32_t len);
/* Returns bytes read (>0), 0 on clean close, -1 on error/timeout. */
int  tls_recv(struct tls_conn *c, void *buf, uint32_t cap, uint32_t timeout_ms);
void tls_close(struct tls_conn *c);
