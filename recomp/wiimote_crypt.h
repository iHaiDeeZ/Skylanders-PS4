// The Wii Remote extension encryption, for bt.cpp: Dolphin's WiimoteEmu/Encryption.h
// (Copyright 2008 Dolphin Emulator Project, Copyright (C) Hector Martin "marcan";
// SPDX-License-Identifier: GPL-2.0-or-later), with its own small integer names.
//
// A game that writes 0xAA to the extension's register 0xF0 and a 16-byte key to 0x40-0x4F reads the
// extension's registers and report bytes encrypted: byte at register address a is
// (x - ft[a % 8]) ^ sb[a % 8], with tables made from the key (wiimote_crypt.cpp).
#pragma once

#include <array>
#include <stdint.h>

namespace WiimoteEmu
{
using u8 = uint8_t;
using u32 = uint32_t;
using SBox = std::array<u8, 256>;

class EncryptionKey
{
public:
	void Encrypt(u8* data, u32 addr, u32 len) const;
	void Decrypt(u8* data, u32 addr, u32 len) const;

	using RandData = std::array<u8, 10>;
	using KeyData = std::array<u8, 6>;

	void GenerateTables(const RandData& rand, const KeyData& key, const SBox& sbox_a, const SBox& sbox_b);

	std::array<u8, 8> ft = {};
	std::array<u8, 8> sb = {};
};

class KeyGen
{
public:
	virtual ~KeyGen() = default;

	using ExtKeyData = std::array<u8, 16>;

	EncryptionKey GenerateFromExtensionKeyData(const ExtKeyData& key_data) const;

protected:
	virtual EncryptionKey::KeyData GenerateKeyData(const EncryptionKey::RandData& rand, u32 idx) const = 0;
	virtual EncryptionKey GenerateTables(const EncryptionKey::RandData& rand, const EncryptionKey::KeyData& key, u32 idx) const = 0;
	virtual EncryptionKey GenerateFallbackTables(const EncryptionKey::RandData& rand, const EncryptionKey::KeyData& key) const = 0;
};

class KeyGen1stParty final : public KeyGen
{
private:
	EncryptionKey::KeyData GenerateKeyData(const EncryptionKey::RandData& rand, u32 idx) const final override;
	EncryptionKey GenerateTables(const EncryptionKey::RandData& rand, const EncryptionKey::KeyData& key, u32 idx) const final override;
	EncryptionKey GenerateFallbackTables(const EncryptionKey::RandData& rand, const EncryptionKey::KeyData& key) const final override;
};

class KeyGen3rdParty final : public KeyGen
{
private:
	EncryptionKey::KeyData GenerateKeyData(const EncryptionKey::RandData& rand, u32 idx) const final override;
	EncryptionKey GenerateTables(const EncryptionKey::RandData& rand, const EncryptionKey::KeyData& key, u32 idx) const final override;
	EncryptionKey GenerateFallbackTables(const EncryptionKey::RandData& rand, const EncryptionKey::KeyData& key) const final override;
};

} // namespace WiimoteEmu
