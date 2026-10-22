#include "GameSpyCrypt.h"

#include <utility>

namespace dice::hfe::net {

// 0x00737e00: a random card below `limit` drawn from the key.
uint8_t GameSpyCrypt::keyRandom(unsigned limit, const uint8_t* key, size_t keySize, uint8_t& sum, size_t& position)
{
	if (limit == 0) {
		return 0;
	}
	unsigned mask = 1;
	while (mask < limit) {
		mask = mask * 2 + 1;
	}
	unsigned retries = 0, value;
	do {
		sum = static_cast<uint8_t>(m_cards[sum] + key[position++]);
		if (position >= keySize) {
			position = 0;
			sum = static_cast<uint8_t>(sum + keySize);
		}
		value = sum & mask;
		if (++retries > 11) {
			value %= limit;
		}
	} while (value > limit);
	return static_cast<uint8_t>(value);
}

GameSpyCrypt::GameSpyCrypt(const uint8_t* key, size_t keySize)
{
	if (!key || keySize == 0) { // 0x00737e90
		for (int i = 0; i < 256; ++i) {
			m_cards[static_cast<size_t>(i)] = static_cast<uint8_t>(255 - i);
		}
		m_rotor = 1;
		m_ratchet = 3;
		m_avalanche = 5;
		m_lastPlain = 7;
		m_lastCipher = 11;
		return;
	}
	for (int i = 0; i < 256; ++i) {
		m_cards[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
	}
	uint8_t sum = 0;
	size_t position = 0;
	for (int i = 255; i >= 0; --i) {
		const uint8_t swap = keyRandom(static_cast<unsigned>(i), key, keySize, sum, position);
		std::swap(m_cards[static_cast<size_t>(i)], m_cards[swap]);
	}
	m_rotor = m_cards[1];
	m_ratchet = m_cards[3];
	m_avalanche = m_cards[5];
	m_lastPlain = m_cards[7];
	m_lastCipher = m_cards[sum];
}

uint8_t GameSpyCrypt::keystream()
{
	auto& c = m_cards;
	m_ratchet = static_cast<uint8_t>(m_ratchet + c[m_rotor++]);
	const uint8_t swap = c[m_lastCipher];
	c[m_lastCipher] = c[m_ratchet];
	c[m_ratchet] = c[m_lastPlain];
	c[m_lastPlain] = c[m_rotor];
	c[m_rotor] = swap;
	m_avalanche = static_cast<uint8_t>(m_avalanche + c[swap]);
	return static_cast<uint8_t>(c[c[static_cast<uint8_t>(c[m_lastPlain] + c[m_lastCipher] + c[m_ratchet])]]
		^ c[static_cast<uint8_t>(c[m_avalanche] + c[m_rotor])]);
}

uint8_t GameSpyCrypt::decrypt(uint8_t cipher)
{
	m_lastPlain = static_cast<uint8_t>(cipher ^ keystream());
	m_lastCipher = cipher;
	return m_lastPlain;
}

uint8_t GameSpyCrypt::encrypt(uint8_t plain)
{
	const uint8_t cipher = static_cast<uint8_t>(plain ^ keystream());
	m_lastPlain = plain;
	m_lastCipher = cipher;
	return cipher;
}

void GameSpyCrypt::decrypt(uint8_t* data, size_t size)
{
	for (size_t i = 0; i < size; ++i) {
		data[i] = decrypt(data[i]);
	}
}

} // namespace dice::hfe::net
