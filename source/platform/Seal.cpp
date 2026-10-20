#include "platform/Seal.hpp"
#include <cstdint>
#include <vector>
#ifdef __SWITCH__
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/platform_util.h>
#include <switch.h>
#else
#define OPENSSL_SUPPRESS_DEPRECATED // EC_KEY reads a raw scalar in a few lines; EVP needs a parameter builder
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#endif
namespace foil::seal {
namespace {
const unsigned char *bytes(const std::string &s) { return reinterpret_cast<const unsigned char *>(s.data()); }
} // namespace
#ifdef __SWITCH__
namespace {
int csrng(void *, unsigned char *out, size_t size) { return R_SUCCEEDED(csrngGetRandomBytes(out, size)) ? 0 : -1; }
// The curve with one key on it, freed whatever happens.
struct Curve {
  mbedtls_ecp_group group;
  mbedtls_mpi d, z;
  mbedtls_ecp_point q;
  Curve() {
    mbedtls_ecp_group_init(&group);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&z);
    mbedtls_ecp_point_init(&q);
  }
  ~Curve() {
    mbedtls_ecp_point_free(&q);
    mbedtls_mpi_free(&z);
    mbedtls_mpi_free(&d);
    mbedtls_ecp_group_free(&group);
  }
};
} // namespace
bool generate(Key &key) {
  Curve c;
  unsigned char secret[ScalarSize], point[PointSize];
  size_t length = 0;
  const bool ok = mbedtls_ecp_group_load(&c.group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                  mbedtls_ecp_gen_keypair(&c.group, &c.d, &c.q, csrng, nullptr) == 0 &&
                  mbedtls_mpi_write_binary(&c.d, secret, sizeof(secret)) == 0 &&
                  mbedtls_ecp_point_write_binary(&c.group, &c.q, MBEDTLS_ECP_PF_UNCOMPRESSED, &length, point,
                                                 sizeof(point)) == 0 &&
                  length == PointSize;
  if (ok) key = {std::string(reinterpret_cast<char *>(secret), ScalarSize), std::string(reinterpret_cast<char *>(point), PointSize)};
  mbedtls_platform_zeroize(secret, sizeof(secret));
  return ok;
}
std::string sharedX(const std::string &secret, const std::string &peerPoint) {
  if (secret.size() != ScalarSize || peerPoint.size() != PointSize) return "";
  Curve c;
  unsigned char x[ScalarSize];
  const bool ok = mbedtls_ecp_group_load(&c.group, MBEDTLS_ECP_DP_SECP256R1) == 0 &&
                  mbedtls_mpi_read_binary(&c.d, bytes(secret), secret.size()) == 0 &&
                  mbedtls_ecp_point_read_binary(&c.group, &c.q, bytes(peerPoint), peerPoint.size()) == 0 &&
                  mbedtls_ecp_check_pubkey(&c.group, &c.q) == 0 &&
                  mbedtls_ecdh_compute_shared(&c.group, &c.z, &c.q, &c.d, csrng, nullptr) == 0 &&
                  mbedtls_mpi_write_binary(&c.z, x, sizeof(x)) == 0;
  std::string out = ok ? std::string(reinterpret_cast<char *>(x), sizeof(x)) : "";
  mbedtls_platform_zeroize(x, sizeof(x));
  return out;
}
bool open(const std::string &key, const std::string &iv, const std::string &aad, const std::string &sealed,
          std::string &plain) {
  if (key.size() != 32 || iv.empty() || sealed.size() < TagSize) return false;
  const size_t size = sealed.size() - TagSize;
  std::vector<unsigned char> out(size + 1);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, bytes(key), 256) == 0 &&
                  mbedtls_gcm_auth_decrypt(&gcm, size, bytes(iv), iv.size(), bytes(aad), aad.size(),
                                           bytes(sealed) + size, TagSize, bytes(sealed), out.data()) == 0;
  mbedtls_gcm_free(&gcm);
  if (ok) plain.assign(reinterpret_cast<char *>(out.data()), size);
  mbedtls_platform_zeroize(out.data(), out.size());
  return ok;
}
bool close(const std::string &key, const std::string &iv, const std::string &aad, const std::string &plain,
           std::string &sealed) {
  if (key.size() != 32 || iv.empty()) return false;
  std::vector<unsigned char> out(plain.size() + TagSize);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, bytes(key), 256) == 0 &&
                  mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, plain.size(), bytes(iv), iv.size(), bytes(aad),
                                            aad.size(), bytes(plain), out.data(), TagSize,
                                            out.data() + plain.size()) == 0;
  mbedtls_gcm_free(&gcm);
  if (ok) sealed.assign(reinterpret_cast<char *>(out.data()), out.size());
  return ok;
}
std::string random(size_t size) {
  std::string out(size, '\0');
  return R_SUCCEEDED(csrngGetRandomBytes(out.data(), size)) ? out : "";
}
#else
namespace {
EC_KEY *keyFor(const std::string &secret) {
  EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  BIGNUM *d = BN_bin2bn(bytes(secret), int(secret.size()), nullptr);
  const bool ok = key && d && EC_KEY_set_private_key(key, d) == 1;
  BN_clear_free(d);
  if (ok) return key;
  EC_KEY_free(key);
  return nullptr;
}
} // namespace
bool generate(Key &key) {
  EC_KEY *k = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  unsigned char secret[ScalarSize], point[PointSize];
  const bool ok = k && EC_KEY_generate_key(k) == 1 &&
                  BN_bn2binpad(EC_KEY_get0_private_key(k), secret, sizeof(secret)) == int(ScalarSize) &&
                  EC_POINT_point2oct(EC_KEY_get0_group(k), EC_KEY_get0_public_key(k), POINT_CONVERSION_UNCOMPRESSED,
                                     point, sizeof(point), nullptr) == PointSize;
  if (ok) key = {std::string(reinterpret_cast<char *>(secret), ScalarSize), std::string(reinterpret_cast<char *>(point), PointSize)};
  EC_KEY_free(k);
  OPENSSL_cleanse(secret, sizeof(secret));
  return ok;
}
std::string sharedX(const std::string &secret, const std::string &peerPoint) {
  if (secret.size() != ScalarSize || peerPoint.size() != PointSize) return "";
  EC_KEY *key = keyFor(secret);
  EC_POINT *q = key ? EC_POINT_new(EC_KEY_get0_group(key)) : nullptr;
  unsigned char x[ScalarSize];
  const bool ok = q && EC_POINT_oct2point(EC_KEY_get0_group(key), q, bytes(peerPoint), peerPoint.size(), nullptr) == 1 &&
                  ECDH_compute_key(x, sizeof(x), q, key, nullptr) == int(sizeof(x));
  EC_POINT_free(q);
  EC_KEY_free(key);
  std::string out = ok ? std::string(reinterpret_cast<char *>(x), sizeof(x)) : "";
  OPENSSL_cleanse(x, sizeof(x));
  return out;
}
bool open(const std::string &key, const std::string &iv, const std::string &aad, const std::string &sealed,
          std::string &plain) {
  if (key.size() != 32 || iv.empty() || sealed.size() < TagSize) return false;
  const int size = int(sealed.size() - TagSize);
  std::vector<unsigned char> out(size_t(size) + 16);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int length = 0, last = 0;
  const bool ok =
      ctx && EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, int(iv.size()), nullptr) == 1 &&
      EVP_DecryptInit_ex(ctx, nullptr, nullptr, bytes(key), bytes(iv)) == 1 &&
      (aad.empty() || EVP_DecryptUpdate(ctx, nullptr, &length, bytes(aad), int(aad.size())) == 1) &&
      EVP_DecryptUpdate(ctx, out.data(), &length, bytes(sealed), size) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, int(TagSize), const_cast<unsigned char *>(bytes(sealed) + size)) == 1 &&
      EVP_DecryptFinal_ex(ctx, out.data() + length, &last) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (ok) plain.assign(reinterpret_cast<char *>(out.data()), size_t(length + last));
  OPENSSL_cleanse(out.data(), out.size());
  return ok;
}
bool close(const std::string &key, const std::string &iv, const std::string &aad, const std::string &plain,
           std::string &sealed) {
  if (key.size() != 32 || iv.empty()) return false;
  std::vector<unsigned char> out(plain.size() + 16 + TagSize);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int length = 0, last = 0;
  const bool ok =
      ctx && EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, int(iv.size()), nullptr) == 1 &&
      EVP_EncryptInit_ex(ctx, nullptr, nullptr, bytes(key), bytes(iv)) == 1 &&
      (aad.empty() || EVP_EncryptUpdate(ctx, nullptr, &length, bytes(aad), int(aad.size())) == 1) &&
      EVP_EncryptUpdate(ctx, out.data(), &length, bytes(plain), int(plain.size())) == 1 &&
      EVP_EncryptFinal_ex(ctx, out.data() + length, &last) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, int(TagSize), out.data() + length + last) == 1;
  EVP_CIPHER_CTX_free(ctx);
  if (ok) sealed.assign(reinterpret_cast<char *>(out.data()), size_t(length + last) + TagSize);
  return ok;
}
std::string random(size_t size) {
  std::string out(size, '\0');
  return RAND_bytes(reinterpret_cast<unsigned char *>(out.data()), int(size)) == 1 ? out : "";
}
#endif
} // namespace foil::seal
