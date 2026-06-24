#pragma once
#include <stdint.h>

/* X.509 certificate-chain validation for the TLS client.
 *
 * tls_verify_chain() parses a TLS Certificate message (1.3 wire format),
 * enforces hostname match (leaf SAN/CN, wildcards), validity dates (vs the
 * RTC), and verifies the cryptographic chain linkage (each cert signed by the
 * next). Trust-anchor policy: if a CA is configured in the trust store the
 * topmost issuer must be a trusted root with a verifying signature; if the
 * store is empty the anchor is left unverified (opportunistic) rather than
 * regressing connectivity. Returns 0 if the chain is acceptable.
 *
 * On success the leaf SubjectPublicKeyInfo (DER) is copied into spki_out so the
 * caller can verify the TLS 1.3 CertificateVerify signature against it. */
int tls_verify_chain(const uint8_t *certmsg, uint32_t len, const char *host,
                     uint8_t *spki_out, uint32_t *spki_len, uint32_t spki_cap);

/* Verify a TLS 1.3 CertificateVerify signature. spki is the leaf public key
 * (DER SubjectPublicKeyInfo); sigalg is the TLS SignatureScheme code; content
 * is the signed data (the 64-space + context-string + transcript-hash blob).
 * Returns 0 if the signature verifies. */
int x509_verify_certverify(const uint8_t *spki, uint32_t spki_len, uint16_t sigalg,
                           const uint8_t *content, uint32_t clen,
                           const uint8_t *sig, uint32_t slen);

/* Install a trust anchor (a root CA certificate, DER). Returns 0 on success. */
int  x509_add_root(const uint8_t *der, uint32_t len);
int  x509_root_count(void);
