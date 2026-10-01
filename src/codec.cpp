// Power Failure Manager -- canonical binary encoding primitives.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/codec.hpp"

#include <array>
#include <cstring>
#include <string>

namespace summon::pfm::codec {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (value >> 1) ^ 0x82F63B78u : value >> 1;
    }
    table[i] = value;
  }
  return table;
}

constexpr auto kCrcTable = make_crc_table();

// FNV-1a 128 over four 32-bit limbs. The prime is 2^88 + 0x13B, so the
// multiply is a bounded schoolbook product that drops everything above 2^128.
struct Fnv128 {
  std::uint32_t limb[4]{0x6295C58Du, 0x62B82175u, 0x07BB0142u, 0x6C62272Eu};

  void xor_byte(std::uint8_t byte) noexcept { limb[0] ^= byte; }

  void mul_prime() noexcept {
    const std::uint32_t prime[4] = {0x0000013Bu, 0u, 0x01000000u, 0u};
    std::uint32_t result[4] = {0u, 0u, 0u, 0u};
    for (int i = 0; i < 4; ++i) {
      std::uint64_t carry = 0;
      for (int j = 0; i + j < 4; ++j) {
        const std::uint64_t current = static_cast<std::uint64_t>(result[i + j]) +
                                      static_cast<std::uint64_t>(limb[i]) * prime[j] + carry;
        result[i + j] = static_cast<std::uint32_t>(current & 0xFFFFFFFFu);
        carry = current >> 32;
      }
    }
    for (int i = 0; i < 4; ++i) {
      limb[i] = result[i];
    }
  }
};

}  // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data) noexcept {
  return crc32c(0xFFFFFFFFu, data) ^ 0xFFFFFFFFu;
}

std::uint32_t crc32c(std::uint32_t seed, std::span<const std::uint8_t> data) noexcept {
  std::uint32_t value = seed;
  for (const auto byte : data) {
    value = kCrcTable[(value ^ byte) & 0xFFu] ^ (value >> 8);
  }
  return value;
}

Fingerprint fnv1a128(std::span<const std::uint8_t> data) noexcept {
  Fnv128 hash;
  for (const auto byte : data) {
    hash.xor_byte(byte);
    hash.mul_prime();
  }
  const std::uint64_t low =
      static_cast<std::uint64_t>(hash.limb[0]) | (static_cast<std::uint64_t>(hash.limb[1]) << 32);
  const std::uint64_t high =
      static_cast<std::uint64_t>(hash.limb[2]) | (static_cast<std::uint64_t>(hash.limb[3]) << 32);
  return Fingerprint{high, low};
}

// --- Writer ---------------------------------------------------------------

void Writer::fail(Status status) {
  if (ok_) {
    ok_ = false;
    error_ = std::move(status);
  }
}

void Writer::u8(std::uint8_t value) { bytes_.push_back(value); }

void Writer::u16(std::uint16_t value) {
  bytes_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  bytes_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void Writer::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Writer::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    bytes_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
  }
}

void Writer::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Writer::flag(bool value) { u8(value ? 1u : 0u); }

void Writer::raw(std::span<const std::uint8_t> data) {
  bytes_.insert(bytes_.end(), data.begin(), data.end());
}

bool Writer::text(const std::string& value, std::size_t max_len) {
  if (value.size() > max_len) {
    fail(Status::error(StatusCode::FieldTooLong, "text exceeds the field bound")
             .with_context("codec.text"));
    return false;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  raw(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()),
                                    value.size()});
  return true;
}

// --- Reader ---------------------------------------------------------------

void Reader::fail(Status status) {
  if (ok_) {
    ok_ = false;
    error_ = std::move(status);
  }
}

bool Reader::need(std::size_t count) {
  if (!ok_) {
    return false;
  }
  if (count > remaining()) {
    fail(Status::error(StatusCode::StoreTruncated, "encoded input ended unexpectedly")
             .with_context("codec.read"));
    return false;
  }
  return true;
}

std::uint8_t Reader::u8() {
  if (!need(1)) {
    return 0;
  }
  return data_[position_++];
}

std::uint16_t Reader::u16() {
  if (!need(2)) {
    return 0;
  }
  std::uint16_t value = 0;
  for (int i = 0; i < 2; ++i) {
    value |= static_cast<std::uint16_t>(data_[position_++]) << (8 * i);
  }
  return value;
}

std::uint32_t Reader::u32() {
  if (!need(4)) {
    return 0;
  }
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(data_[position_++]) << (8 * i);
  }
  return value;
}

std::uint64_t Reader::u64() {
  if (!need(8)) {
    return 0;
  }
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(data_[position_++]) << (8 * i);
  }
  return value;
}

std::int64_t Reader::i64() { return static_cast<std::int64_t>(u64()); }

bool Reader::flag() {
  const auto value = u8();
  if (!ok_) {
    return false;
  }
  if (value > 1u) {
    fail(Status::error(StatusCode::MalformedEncoding, "boolean field is not zero or one")
             .with_context("codec.flag"));
    return false;
  }
  return value == 1u;
}

std::span<const std::uint8_t> Reader::raw(std::size_t count) {
  if (!need(count)) {
    return {};
  }
  const auto view = data_.subspan(position_, count);
  position_ += count;
  return view;
}

bool Reader::text(std::string& out, std::size_t max_len) {
  const auto declared = u32();
  if (!ok_) {
    return false;
  }
  // The declared length is checked against the structural bound before any
  // allocation is attempted, so an absurd length cannot exhaust memory.
  if (declared > max_len) {
    fail(Status::error(StatusCode::FieldTooLong, "declared text length exceeds the field bound")
             .with_context("codec.text"));
    return false;
  }
  const auto view = raw(declared);
  if (!ok_) {
    return false;
  }
  out.assign(reinterpret_cast<const char*>(view.data()), view.size());
  return true;
}

}  // namespace summon::pfm::codec
