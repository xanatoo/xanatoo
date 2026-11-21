// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// TlsStream over Mbed TLS 3.x (Android; desktop with -DXANATOO_TLS=mbedtls).
// Uses the caller's socket through send/recv callbacks so its SO_RCVTIMEO /
// SO_SNDTIMEO keep working.

#include "xanatoo/tls.hpp"

#include "xanatoo/paths.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/asn1.h>
#include <mbedtls/ecp.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace xanatoo {

namespace {

#ifdef MBEDTLS_ENTROPY_HARDWARE_ALT
// Entropy source when the platform one is disabled (Android build, see
// mk/android/mbedtls/Android.mk): /dev/urandom, which never blocks, unlike
// getrandom() on old kernels.
extern "C" int mbedtls_hardware_poll(void*, unsigned char* output, size_t len,
                                     size_t* olen) {
  *olen = 0;
  FILE* f = std::fopen("/dev/urandom", "rb");
  if (!f) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  *olen = std::fread(output, 1, len, f);
  std::fclose(f);
  return *olen == len ? 0 : MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
}
#endif

int send_cb(void* ctx, const unsigned char* buf, size_t len) {
  const int fd = *static_cast<int*>(ctx);
  const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
  if (n >= 0) return static_cast<int>(n);
  if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
  return MBEDTLS_ERR_NET_SEND_FAILED;
}

int recv_cb(void* ctx, unsigned char* buf, size_t len) {
  const int fd = *static_cast<int*>(ctx);
  const ssize_t n = ::recv(fd, buf, len, 0);
  if (n >= 0) return static_cast<int>(n);  // 0 = peer closed
  if (errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
  // SO_RCVTIMEO expired: fail like OpenSSL does instead of spinning.
  if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_TIMEOUT;
  return MBEDTLS_ERR_NET_RECV_FAILED;
}

std::string error_text(int rc) {
  char buf[160];
  mbedtls_strerror(rc, buf, sizeof(buf));
  return buf;
}

std::string certificate_name(const mbedtls_x509_name* name) {
  if (!name) return {};
  char buffer[1024];
  const int count = mbedtls_x509_dn_gets(buffer, sizeof(buffer), name);
  return count >= 0 ? std::string(buffer) : std::string{};
}

std::string certificate_time(const mbedtls_x509_time& time) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d UTC",
                time.year, time.mon, time.day, time.hour, time.min, time.sec);
  return buffer;
}

// Loading hundreds of Android CA files once per HTTPS image/request is
// prohibitively expensive. The parsed chain is read-only after construction
// and shared by all verified TLS connections.
//
// The system store comes first; the bundled Mozilla roots
// (data_file("certs/ca-bundle.crt"), shipped in the APK) are added on top.
// Old Android releases froze their store years ago: Android 5 lacks ISRG
// Root X1 (Let's Encrypt, added in 7.1.1) and Mbed TLS, unlike the platform
// stack, rejects the expired DST Root CA X3 cross-sign.
struct SystemTrust {
  mbedtls_x509_crt chain;
  bool available = false;

  SystemTrust() {
    mbedtls_x509_crt_init(&chain);
#if defined(MBEDTLS_FS_IO)
    if (load_system()) available = true;
    const std::string bundled = data_file("certs/ca-bundle.crt");
    if (!bundled.empty() &&
        mbedtls_x509_crt_parse_file(&chain, bundled.c_str()) >= 0)
      available = true;
#endif
  }

#if defined(MBEDTLS_FS_IO)
  // The first system store that parses (files, then hashed directories).
  bool load_system() {
    for (const char* variable : {"SSL_CERT_FILE", "NIX_SSL_CERT_FILE"}) {
      const char* path = std::getenv(variable);
      if (path && *path && mbedtls_x509_crt_parse_file(&chain, path) >= 0)
        return true;
    }
    for (const char* path : {"/etc/ssl/certs/ca-certificates.crt",
                             "/etc/ssl/cert.pem",
                             "/etc/pki/tls/certs/ca-bundle.crt"}) {
      if (mbedtls_x509_crt_parse_file(&chain, path) >= 0) return true;
    }
    if (const char* path = std::getenv("SSL_CERT_DIR");
        path && *path && mbedtls_x509_crt_parse_path(&chain, path) >= 0)
      return true;
    for (const char* path : {"/apex/com.android.conscrypt/cacerts",
                             "/system/etc/security/cacerts",
                             "/etc/ssl/certs"}) {
      if (mbedtls_x509_crt_parse_path(&chain, path) >= 0) return true;
    }
    return false;
  }
#endif

  ~SystemTrust() { mbedtls_x509_crt_free(&chain); }
};

SystemTrust& system_trust() {
  static SystemTrust trust;  // C++11 thread-safe initialization
  return trust;
}

// Why verification failed, for the error message: the reasons from the
// verify flags ("not correctly signed by the trusted CA", "expired", ...)
// and, for an untrusted chain, where it ends: the root the store lacks.
std::string verify_detail(uint32_t flags, const std::string& chain_top) {
  std::string out;
  if (flags != 0) {
    char buf[512];
    if (mbedtls_x509_crt_verify_info(buf, sizeof(buf), "", flags) > 0) {
      std::string_view text(buf);
      while (!text.empty()) {
        const size_t nl = text.find('\n');
        const std::string_view line = text.substr(0, nl);
        if (!line.empty()) {
          if (!out.empty()) out += "; ";
          out += line;
        }
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
      }
    }
  }
  if ((flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED) && !chain_top.empty()) {
    if (!out.empty()) out += "; ";
    out += "chain ends at issuer " + chain_top;
  }
  return out;
}

// A seeded CTR-DRBG for key generation and key parsing outside a stream.
struct Rng {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool ok = false;
  Rng() {
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    static const char pers[] = "xanatoo-identity";
    ok = psa_crypto_init() == PSA_SUCCESS &&
         mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                               reinterpret_cast<const unsigned char*>(pers),
                               sizeof(pers) - 1) == 0;
  }
  ~Rng() {
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
  }
  Rng(const Rng&) = delete;
  Rng& operator=(const Rng&) = delete;
};

struct Cert {
  mbedtls_x509_crt crt;
  Cert() { mbedtls_x509_crt_init(&crt); }
  ~Cert() { mbedtls_x509_crt_free(&crt); }
  Cert(const Cert&) = delete;
  Cert& operator=(const Cert&) = delete;
  // PEM parsing needs the terminating NUL in the length.
  bool parse(const std::string& pem) {
    return mbedtls_x509_crt_parse(
               &crt, reinterpret_cast<const unsigned char*>(pem.c_str()),
               pem.size() + 1) == 0;
  }
};

struct Key {
  mbedtls_pk_context pk;
  Key() { mbedtls_pk_init(&pk); }
  ~Key() { mbedtls_pk_free(&pk); }
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
  bool parse(const std::string& pem, mbedtls_ctr_drbg_context& drbg) {
    return mbedtls_pk_parse_key(
               &pk, reinterpret_cast<const unsigned char*>(pem.c_str()),
               pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random,
               &drbg) == 0;
  }
};

std::optional<std::string> sha256_of(const mbedtls_x509_crt* cert) {
  if (!cert || !cert->raw.p || !cert->raw.len) return std::nullopt;
  unsigned char digest[32];
  size_t size = 0;
  if (psa_crypto_init() != PSA_SUCCESS ||
      psa_hash_compute(PSA_ALG_SHA_256, cert->raw.p, cert->raw.len,
                       digest, sizeof(digest), &size) != PSA_SUCCESS ||
      size != sizeof(digest)) return std::nullopt;
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    result += hex[digest[i] >> 4];
    result += hex[digest[i] & 15];
  }
  return result;
}

TlsPeerCertificate details_of(const mbedtls_x509_crt* cert) {
  TlsPeerCertificate details;
  details.subject = certificate_name(&cert->subject);
  details.issuer = certificate_name(&cert->issuer);
  details.not_before = certificate_time(cert->valid_from);
  details.not_after = certificate_time(cert->valid_to);
  details.self_issued = !details.subject.empty() &&
                        details.subject == details.issuer;
  // The Mbed TLS public certificate API exposes matching names, but that
  // alone does not prove the self-signature. Report it as unknown rather than
  // claim the certificate has a valid self-signature without verifying it.
  return details;
}

// "CN=…" for mbedtls_x509write_crt_set_subject_name: at most 64 bytes (cut
// at a UTF-8 boundary), the DN special characters escaped.
std::string common_name_dn(std::string cn) {
  if (cn.empty()) cn = "xanatoo";
  if (cn.size() > 64) {
    size_t n = 64;
    while (n > 0 && (static_cast<unsigned char>(cn[n]) & 0xC0) == 0x80) --n;
    cn.resize(n);
  }
  std::string out = "CN=";
  for (char c : cn) {
    if (std::strchr(",=+<>#;\\\"", c)) out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

struct TlsStream::Impl {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_config conf;
  mbedtls_ssl_context ssl;
  mbedtls_x509_crt own_cert;  // the client identity (live while `live`)
  mbedtls_pk_context own_key;
  int fd = -1;
  bool live = false;  // contexts initialised (free them in close())
  bool ok = false;    // handshake done
  std::string last_error;
  std::string chain_top;  // issuer of the topmost presented certificate
  TlsClientIdentity client;
  std::function<bool(const std::string&)> guard;
  bool guard_refused = false;
};

std::optional<TlsClientIdentity> tls_make_client_identity(
    const std::string& common_name, std::string* err) {
  auto fail = [&](const char* what, int rc) -> std::optional<TlsClientIdentity> {
    if (err) *err = std::string(what) + (rc ? ": " + error_text(rc) : "");
    return std::nullopt;
  };
  Rng rng;
  if (!rng.ok) return fail("TLS RNG seed failed", 0);
  Key key;
  int rc = mbedtls_pk_setup(&key.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (rc == 0)
    rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key.pk),
                             mbedtls_ctr_drbg_random, &rng.drbg);
  if (rc != 0) return fail("cannot generate an EC P-256 key", rc);

  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  struct Done {
    mbedtls_x509write_cert* c;
    ~Done() { mbedtls_x509write_crt_free(c); }
  } done{&crt};
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &key.pk);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key.pk);
  const std::string dn = common_name_dn(common_name);
  if ((rc = mbedtls_x509write_crt_set_subject_name(&crt, dn.c_str())) != 0 ||
      (rc = mbedtls_x509write_crt_set_issuer_name(&crt, dn.c_str())) != 0)
    return fail("cannot set the certificate name", rc);
  unsigned char serial[16];
  if ((rc = mbedtls_ctr_drbg_random(&rng.drbg, serial, sizeof serial)) != 0)
    return fail("no random bytes for the certificate serial", rc);
  serial[0] = static_cast<unsigned char>((serial[0] & 0x7f) | 0x40);
  if ((rc = mbedtls_x509write_crt_set_serial_raw(&crt, serial,
                                                 sizeof serial)) != 0)
    return fail("cannot set the certificate serial", rc);
  char from[16];
  const std::time_t now = std::time(nullptr);
  std::tm utc{};
  gmtime_r(&now, &utc);
  std::strftime(from, sizeof from, "%Y%m%d%H%M%S", &utc);
  if ((rc = mbedtls_x509write_crt_set_validity(&crt, from,
                                               "99991231235959")) != 0)
    return fail("cannot set the certificate validity", rc);
  if ((rc = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) != 0)
    return fail("cannot add a certificate extension", rc);
  mbedtls_asn1_sequence client_auth{};
  client_auth.buf.tag = MBEDTLS_ASN1_OID;
  client_auth.buf.p = reinterpret_cast<unsigned char*>(
      const_cast<char*>(MBEDTLS_OID_CLIENT_AUTH));
  client_auth.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_CLIENT_AUTH);
  if ((rc = mbedtls_x509write_crt_set_ext_key_usage(&crt, &client_auth)) != 0)
    return fail("cannot add a certificate extension", rc);
  unsigned char buf[4096];
  if ((rc = mbedtls_x509write_crt_pem(&crt, buf, sizeof buf,
                                      mbedtls_ctr_drbg_random,
                                      &rng.drbg)) != 0)
    return fail("cannot encode the certificate", rc);
  TlsClientIdentity out;
  out.cert_pem = reinterpret_cast<const char*>(buf);
  if ((rc = mbedtls_pk_write_key_pem(&key.pk, buf, sizeof buf)) != 0)
    return fail("cannot encode the private key", rc);
  out.key_pem = reinterpret_cast<const char*>(buf);
  std::memset(buf, 0, sizeof buf);
  return out;
}

std::optional<std::string> tls_certificate_sha256(const std::string& cert_pem) {
  Cert cert;
  if (!cert.parse(cert_pem)) return std::nullopt;
  return sha256_of(&cert.crt);
}

std::optional<TlsPeerCertificate> tls_certificate_details(
    const std::string& cert_pem) {
  Cert cert;
  if (!cert.parse(cert_pem)) return std::nullopt;
  return details_of(&cert.crt);
}

bool tls_identity_matches(const TlsClientIdentity& identity, std::string* err) {
  Rng rng;
  Cert cert;
  Key key;
  if (!rng.ok) {
    if (err) *err = "TLS RNG seed failed";
    return false;
  }
  if (!cert.parse(identity.cert_pem)) {
    if (err) *err = "no PEM certificate";
    return false;
  }
  if (!key.parse(identity.key_pem, rng.drbg)) {
    if (err) *err = "no PEM private key (an encrypted key needs its "
                    "passphrase removed first)";
    return false;
  }
  if (mbedtls_pk_check_pair(&cert.crt.pk, &key.pk, mbedtls_ctr_drbg_random,
                            &rng.drbg) != 0) {
    if (err) *err = "the private key does not belong to the certificate";
    return false;
  }
  return true;
}

void TlsStream::set_client_identity(TlsClientIdentity identity) {
  impl_->client = std::move(identity);
}

void TlsStream::set_peer_guard(
    std::function<bool(const std::string& sha256)> guard) {
  impl_->guard = std::move(guard);
}

bool TlsStream::peer_guard_refused() const { return impl_->guard_refused; }

const std::string& TlsStream::last_error() const { return impl_->last_error; }

TlsStream::TlsStream() : impl_(std::make_unique<Impl>()) {}
TlsStream::~TlsStream() { close(); }

const char* TlsStream::backend() { return "Mbed TLS"; }

// Linked statically, so the runtime version is the built one.
std::string TlsStream::version() { return MBEDTLS_VERSION_STRING_FULL; }

bool TlsStream::connect(int fd, const std::string& host, std::string* err,
                        bool verify_peer) {
  close();
  Impl& m = *impl_;
  m.fd = fd;
  auto fail = [&](const std::string& what, int rc,
                  const std::string& detail = {}) {
    if (err)
      *err = what + ": " + error_text(rc) +
             (detail.empty() ? "" : ": " + detail) + " (" + host + ")";
    close();
    return false;
  };
  // TLS 1.3 in Mbed TLS 3.x runs on PSA crypto; idempotent.
  if (const psa_status_t ps = psa_crypto_init(); ps != PSA_SUCCESS) {
    if (err) *err = "psa_crypto_init failed (" + std::to_string(ps) + ")";
    close();
    return false;
  }
  mbedtls_entropy_init(&m.entropy);
  mbedtls_ctr_drbg_init(&m.drbg);
  mbedtls_ssl_config_init(&m.conf);
  mbedtls_ssl_init(&m.ssl);
  mbedtls_x509_crt_init(&m.own_cert);
  mbedtls_pk_init(&m.own_key);
  m.live = true;

  static const char pers[] = "xanatoo-tls";
  int rc = mbedtls_ctr_drbg_seed(&m.drbg, mbedtls_entropy_func, &m.entropy,
                                 reinterpret_cast<const unsigned char*>(pers),
                                 sizeof(pers) - 1);
  if (rc != 0) return fail("TLS RNG seed failed", rc);
  rc = mbedtls_ssl_config_defaults(&m.conf, MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc != 0) return fail("TLS config failed", rc);
  mbedtls_ssl_conf_min_tls_version(&m.conf, MBEDTLS_SSL_VERSION_TLS1_2);
  if (verify_peer) {
    SystemTrust& roots = system_trust();
    if (!roots.available) {
      if (err) *err = "no system TLS CA store (set SSL_CERT_FILE)";
      close();
      return false;
    }
    mbedtls_ssl_conf_authmode(&m.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&m.conf, &roots.chain, nullptr);
    m.chain_top.clear();
    mbedtls_ssl_conf_verify(
        &m.conf,
        [](void* ctx, mbedtls_x509_crt* crt, int, uint32_t*) {
          // Called top of the chain first, so the first call names the root
          // (or the last intermediate the server sent).
          auto& top = *static_cast<std::string*>(ctx);
          if (top.empty()) top = certificate_name(&crt->issuer);
          return 0;
        },
        &m.chain_top);
  } else if (m.guard) {
    // OPTIONAL runs the verify callback on the server's certificate (before
    // the client sends its own); the callback clears the chain flags, so
    // only the guard decides. FATAL_ERROR, unlike CERT_VERIFY_FAILED, is not
    // forgiven by OPTIONAL and aborts the handshake.
    m.guard_refused = false;
    mbedtls_ssl_conf_authmode(&m.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_verify(
        &m.conf,
        [](void* ctx, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
          *flags = 0;
          if (depth != 0) return 0;
          auto& impl = *static_cast<Impl*>(ctx);
          const auto sha = sha256_of(crt);
          if (sha && impl.guard(*sha)) return 0;
          impl.guard_refused = true;
          return MBEDTLS_ERR_X509_FATAL_ERROR;
        },
        &m);
  } else {
    mbedtls_ssl_conf_authmode(&m.conf, MBEDTLS_SSL_VERIFY_NONE);
  }
  mbedtls_ssl_conf_rng(&m.conf, mbedtls_ctr_drbg_random, &m.drbg);
  if (!m.client.cert_pem.empty()) {
    // Sent only when the server's handshake asks for a certificate.
    const std::string& cpem = m.client.cert_pem;
    const std::string& kpem = m.client.key_pem;
    rc = mbedtls_x509_crt_parse(
        &m.own_cert, reinterpret_cast<const unsigned char*>(cpem.c_str()),
        cpem.size() + 1);
    if (rc == 0)
      rc = mbedtls_pk_parse_key(
          &m.own_key, reinterpret_cast<const unsigned char*>(kpem.c_str()),
          kpem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &m.drbg);
    if (rc == 0) rc = mbedtls_ssl_conf_own_cert(&m.conf, &m.own_cert, &m.own_key);
    if (rc != 0) return fail("client certificate does not load", rc);
  }
  rc = mbedtls_ssl_setup(&m.ssl, &m.conf);
  if (rc != 0) return fail("TLS setup failed", rc);
  rc = mbedtls_ssl_set_hostname(&m.ssl, host.c_str());  // SNI
  if (rc != 0) return fail("TLS SNI failed", rc);
  mbedtls_ssl_set_bio(&m.ssl, &m.fd, send_cb, recv_cb, nullptr);

  while ((rc = mbedtls_ssl_handshake(&m.ssl)) != 0) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
      const std::string why =
          verify_detail(mbedtls_ssl_get_verify_result(&m.ssl), m.chain_top);
      if (!why.empty())
        return fail("TLS handshake failed", rc, why);
    }
    return fail("TLS handshake failed", rc);
  }
  if (verify_peer && mbedtls_ssl_get_verify_result(&m.ssl) != 0) {
    if (err) *err = "TLS certificate or hostname verification failed (" + host + ")";
    close();
    return false;
  }
  m.ok = true;
  return true;
}

bool TlsStream::connected() const { return impl_->ok; }

std::optional<std::string> TlsStream::peer_certificate_sha256() const {
  if (!connected()) return std::nullopt;
  return sha256_of(mbedtls_ssl_get_peer_cert(&impl_->ssl));
}

std::optional<TlsPeerCertificate> TlsStream::peer_certificate_details() const {
  if (!connected()) return std::nullopt;
  const mbedtls_x509_crt* cert = mbedtls_ssl_get_peer_cert(&impl_->ssl);
  if (!cert) return std::nullopt;
  return details_of(cert);
}

bool TlsStream::write_all(const void* data, size_t n) {
  if (!impl_->ok) return false;
  const unsigned char* p = static_cast<const unsigned char*>(data);
  while (n) {
    const int r = mbedtls_ssl_write(&impl_->ssl, p, n);
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

int TlsStream::read(void* buf, size_t n) {
  if (!impl_->ok) return -1;
  for (;;) {
    const int r =
        mbedtls_ssl_read(&impl_->ssl, static_cast<unsigned char*>(buf), n);
    if (r > 0) return r;
    if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    // TLS 1.3 post-handshake messages: not data, read on.
    if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
    char msg[160];
    mbedtls_strerror(r, msg, sizeof(msg));
    impl_->last_error = std::string(msg) + " (-0x" + [&] {
      char h[16]; std::snprintf(h, sizeof(h), "%04x", -r); return std::string(h);
    }() + ")";
    return -1;
  }
}

bool TlsStream::pending() const {
  return impl_->ok && mbedtls_ssl_get_bytes_avail(&impl_->ssl) > 0;
}

void TlsStream::close() {
  Impl& m = *impl_;
  if (m.ok) mbedtls_ssl_close_notify(&m.ssl);
  if (m.live) {
    mbedtls_ssl_free(&m.ssl);
    mbedtls_ssl_config_free(&m.conf);
    mbedtls_pk_free(&m.own_key);
    mbedtls_x509_crt_free(&m.own_cert);
    mbedtls_ctr_drbg_free(&m.drbg);
    mbedtls_entropy_free(&m.entropy);
  }
  m.live = m.ok = false;
  if (m.fd >= 0) {
    ::close(m.fd);
    m.fd = -1;
  }
}

}  // namespace xanatoo
