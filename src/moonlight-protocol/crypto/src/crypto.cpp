#include "aes.hpp"
#include "sign.hpp"
#include <algorithm>
#include <iomanip>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <sstream>
#include <stdexcept>

namespace crypto {

std::string sha256(std::string_view str) {
  unsigned char md_value[EVP_MAX_MD_SIZE];
  unsigned int md_len;
  signature::EVP_MD_CTX_ptr mdctx(EVP_MD_CTX_new(), ::EVP_MD_CTX_free);
  if (!mdctx)
    handle_openssl_error("EVP_MD_CTX_new failed");

  if (EVP_DigestInit_ex(mdctx.get(), EVP_sha256(), nullptr) != 1)
    handle_openssl_error("EVP_DigestInit_ex failed");
  if (EVP_DigestUpdate(mdctx.get(), str.data(), str.size()) != 1)
    handle_openssl_error("EVP_DigestUpdate failed");
  if (EVP_DigestFinal_ex(mdctx.get(), md_value, &md_len) != 1)
    handle_openssl_error("EVP_DigestFinal_ex failed");

  std::stringstream ss;
  for (int i = 0; i < md_len; ++i) {
    ss << std::hex << std::setw(2) << std::setfill('0') << (int)md_value[i];
  }
  return ss.str();
}

std::string str_to_hex(std::string_view input) {
  static const char hex_digits[] = "0123456789ABCDEF";

  std::string output;
  output.reserve(input.length() * 2);
  for (unsigned char c : input) {
    output.push_back(hex_digits[c >> 4]);
    output.push_back(hex_digits[c & 15]);
  }
  return output;
}

std::string hex_to_str(std::string_view hex, bool reverse = true) {
  if (hex.empty())
    return {};
  std::string buf;

  static char constexpr shift_bit = 'a' - 'A';
  auto is_convertable = [](char ch) -> bool {
    if (isdigit(static_cast<unsigned char>(ch))) {
      return true;
    }
    ch |= shift_bit;
    if ('a' > ch || ch > 'z') {
      return false;
    }
    return true;
  };

  auto buf_size = std::count_if(std::begin(hex), std::end(hex), is_convertable) / 2;
  buf.resize(buf_size);

  const char *data = hex.data() + hex.size() - 1;

  auto convert = [](char ch) -> std::uint8_t {
    if (ch >= '0' && ch <= '9') {
      return (std::uint8_t)ch - '0';
    }
    return (std::uint8_t)(ch | (char)32) - 'a' + (char)10;
  };

  for (auto &el : buf) {
    while (!is_convertable(*data)) {
      --data;
    }
    std::uint8_t ch_r = convert(*data--);
    while (!is_convertable(*data)) {
      --data;
    }
    std::uint8_t ch_l = convert(*data--);
    el = (ch_l << 4) | ch_r;
  }

  if (reverse) {
    std::reverse(std::begin(buf), std::end(buf));
  }

  return buf;
}

std::string random(int length) {
  std::string rnd;
  rnd.resize(length);
  if (RAND_bytes((uint8_t *)rnd.data(), length) != 1)
    handle_openssl_error("RAND_bytes failed");
  return rnd;
}

std::string aes_encrypt_ecb(std::string_view msg, std::string_view enc_key, std::string_view iv, bool padding) {
  auto ctx = aes::init(EVP_aes_128_ecb(), enc_key, iv, true, padding);
  return aes::encrypt_symmetric(ctx.get(), msg);
}

std::string aes_decrypt_ecb(std::string_view msg, std::string_view enc_key, std::string_view iv, bool padding) {
  auto ctx = aes::init(EVP_aes_128_ecb(), enc_key, iv, false, padding);
  return aes::decrypt_symmetric(ctx.get(), msg);
}

std::string aes_encrypt_cbc(std::string_view msg, std::string_view enc_key, std::string_view iv, bool padding) {
  auto ctx = aes::init(EVP_aes_128_cbc(), enc_key, iv, true, padding);
  return aes::encrypt_symmetric(ctx.get(), msg);
}

std::string aes_decrypt_cbc(std::string_view msg, std::string_view enc_key, std::string_view iv, bool padding) {
  auto ctx = aes::init(EVP_aes_128_cbc(), enc_key, iv, false, padding);
  return aes::decrypt_symmetric(ctx.get(), msg);
}

// Set the IV length before passing its memory to OpenSSL. Initialising with
// the default (12-byte) IV first can overread a shorter caller-provided buffer.
static aes::CIPHER_CTX_ptr
gcm_context(std::string_view key, std::string_view iv, int iv_size, bool encrypt, bool padding) {
  if (iv_size == -1)
    iv_size = static_cast<int>(iv.size());
  if (key.size() != 16 || iv_size <= 0 || static_cast<size_t>(iv_size) > iv.size())
    throw std::runtime_error("Invalid AES-GCM key or IV length");
  aes::CIPHER_CTX_ptr ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx || EVP_CipherInit_ex(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr, nullptr, encrypt) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, iv_size, nullptr) != 1 ||
      EVP_CipherInit_ex(ctx.get(),
                        nullptr,
                        nullptr,
                        reinterpret_cast<const unsigned char *>(key.data()),
                        reinterpret_cast<const unsigned char *>(iv.data()),
                        encrypt) != 1 ||
      EVP_CIPHER_CTX_set_padding(ctx.get(), padding) != 1)
    handle_openssl_error("AES-GCM initialization failed");
  return ctx;
}

std::pair<std::string, std::string> aes_encrypt_gcm(std::string_view msg,
                                                    std::string_view enc_key,
                                                    std::string_view iv = random(AES_BLOCK_SIZE),
                                                    int iv_size = -1,
                                                    bool padding = false) {
  auto ctx = gcm_context(enc_key, iv, iv_size, true, padding);
  return aes::encrypt_authenticated(ctx.get(), msg);
}

std::string aes_decrypt_gcm(std::string_view msg,
                            std::string_view enc_key,
                            std::string_view tag,
                            std::string_view iv = random(AES_BLOCK_SIZE),
                            int iv_size = -1,
                            bool padding = false) {
  auto ctx = gcm_context(enc_key, iv, iv_size, false, padding);
  return aes::decrypt_authenticated(ctx.get(), msg, tag);
}

std::string sign(std::string_view msg, std::string_view private_key) {
  auto p_key = signature::create_key(private_key, true);
  return signature::sign(msg, p_key.get(), EVP_sha256());
}

bool verify(std::string_view msg, std::string_view signature, std::string_view public_key) {
  // An unparseable or unusable public key can't have produced a valid signature
  try {
    auto p_key = signature::create_key(public_key, false);
    return signature::verify(msg, signature, p_key.get(), EVP_sha256());
  } catch (const std::runtime_error &) {
    return false;
  }
}

} // namespace crypto