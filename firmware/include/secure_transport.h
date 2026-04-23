#pragma once

#include <Arduino.h>
#include <mbedtls/gcm.h>
#include <esp_system.h>

#include "lora_config.h"

namespace lora_app {

constexpr uint8_t SECURE_FRAME_MAGIC = 0xA5;
constexpr uint8_t SECURE_FRAME_VERSION = 0x02;
constexpr size_t SECURE_AES_KEY_SIZE = 16;
constexpr size_t SECURE_NONCE_SIZE = 12;
constexpr size_t SECURE_TAG_SIZE = 16;
constexpr size_t SECURE_HEADER_SIZE = 4 + SECURE_NONCE_SIZE;
constexpr size_t SECURE_MAX_FRAME_SIZE = 128;

inline bool secure_hex_nibble(char c, uint8_t &nibble) {
  if (c >= '0' && c <= '9') {
    nibble = static_cast<uint8_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    nibble = static_cast<uint8_t>(10 + (c - 'a'));
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    nibble = static_cast<uint8_t>(10 + (c - 'A'));
    return true;
  }
  return false;
}

inline bool secure_get_key(uint8_t key[SECURE_AES_KEY_SIZE]) {
  static bool loaded = false;
  static bool valid = false;
  static uint8_t cachedKey[SECURE_AES_KEY_SIZE]{};

  if (!loaded) {
    loaded = true;
    const char *hex = LORA_AES_KEY_HEX;
    if (hex == nullptr || strlen(hex) != SECURE_AES_KEY_SIZE * 2) {
      valid = false;
    } else {
      valid = true;
      for (size_t i = 0; i < SECURE_AES_KEY_SIZE; i++) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!secure_hex_nibble(hex[i * 2], hi) || !secure_hex_nibble(hex[(i * 2) + 1], lo)) {
          valid = false;
          break;
        }
        cachedKey[i] = static_cast<uint8_t>((hi << 4) | lo);
      }
    }
  }

  if (!valid) {
    return false;
  }

  memcpy(key, cachedKey, SECURE_AES_KEY_SIZE);
  return true;
}

inline bool secure_gcm_encrypt(const uint8_t *plain, size_t plainLen, const uint8_t nonce[SECURE_NONCE_SIZE],
                               const uint8_t *aad, size_t aadLen, uint8_t *cipherOut,
                               uint8_t tagOut[SECURE_TAG_SIZE]) {
  uint8_t key[SECURE_AES_KEY_SIZE]{};
  if (!secure_get_key(key)) {
    return false;
  }

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, SECURE_AES_KEY_SIZE * 8);
  if (rc != 0) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  rc = mbedtls_gcm_crypt_and_tag(&gcm,
                                 MBEDTLS_GCM_ENCRYPT,
                                 plainLen,
                                 nonce,
                                 SECURE_NONCE_SIZE,
                                 aad,
                                 aadLen,
                                 plain,
                                 cipherOut,
                                 SECURE_TAG_SIZE,
                                 tagOut);
  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

inline bool secure_gcm_decrypt(const uint8_t *cipher, size_t cipherLen, const uint8_t nonce[SECURE_NONCE_SIZE],
                               const uint8_t *aad, size_t aadLen, const uint8_t tag[SECURE_TAG_SIZE],
                               uint8_t *plainOut) {
  uint8_t key[SECURE_AES_KEY_SIZE]{};
  if (!secure_get_key(key)) {
    return false;
  }

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  int rc = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, SECURE_AES_KEY_SIZE * 8);
  if (rc != 0) {
    mbedtls_gcm_free(&gcm);
    return false;
  }

  rc = mbedtls_gcm_auth_decrypt(
      &gcm, cipherLen, nonce, SECURE_NONCE_SIZE, aad, aadLen, tag, SECURE_TAG_SIZE, cipher, plainOut);
  mbedtls_gcm_free(&gcm);
  return rc == 0;
}

inline bool secure_encrypt_frame(const uint8_t *plain, size_t plainLen, uint8_t *out, size_t outCapacity, size_t &outLen) {
  outLen = 0;
  if (plain == nullptr || out == nullptr || plainLen == 0 || plainLen > 255) {
    return false;
  }

  const size_t frameLen = SECURE_HEADER_SIZE + plainLen + SECURE_TAG_SIZE;
  if (frameLen > outCapacity || frameLen > SECURE_MAX_FRAME_SIZE) {
    return false;
  }

  out[0] = SECURE_FRAME_MAGIC;
  out[1] = SECURE_FRAME_VERSION;
  out[2] = static_cast<uint8_t>(plainLen & 0xFF);
  out[3] = static_cast<uint8_t>((plainLen >> 8) & 0xFF);

  uint8_t *nonce = out + 4;
  esp_fill_random(nonce, SECURE_NONCE_SIZE);

  uint8_t *cipher = out + SECURE_HEADER_SIZE;
  uint8_t *tag = cipher + plainLen;
  if (!secure_gcm_encrypt(plain, plainLen, nonce, out, 4, cipher, tag)) {
    return false;
  }

  outLen = frameLen;
  return true;
}

inline bool secure_decrypt_frame(const uint8_t *in, size_t inLen, uint8_t *plainOut, size_t plainCapacity, size_t &plainLen) {
  plainLen = 0;
  if (in == nullptr || plainOut == nullptr || inLen < (SECURE_HEADER_SIZE + SECURE_TAG_SIZE)) {
    return false;
  }

  if (in[0] != SECURE_FRAME_MAGIC || in[1] != SECURE_FRAME_VERSION) {
    return false;
  }

  const size_t payloadLen = static_cast<size_t>(in[2]) | (static_cast<size_t>(in[3]) << 8);
  if (payloadLen == 0 || payloadLen > plainCapacity) {
    return false;
  }

  if (SECURE_HEADER_SIZE + payloadLen + SECURE_TAG_SIZE != inLen) {
    return false;
  }

  const uint8_t *nonce = in + 4;
  const uint8_t *cipher = in + SECURE_HEADER_SIZE;
  const uint8_t *tag = cipher + payloadLen;
  if (!secure_gcm_decrypt(cipher, payloadLen, nonce, in, 4, tag, plainOut)) {
    memset(plainOut, 0, plainCapacity);
    return false;
  }

  plainLen = payloadLen;
  return true;
}

}  // namespace lora_app
