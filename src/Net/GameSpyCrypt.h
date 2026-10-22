// GameSpy's stream cipher ("GOACrypt"), used by the master server's list
// replies (BF2142.exe: init 0x00737ed0, key schedule 0x00737e00, decrypt
// 0x00737f80 / 0x00738070). A 256-entry card table shuffled by the key,
// and five indices (rotor, ratchet, avalanche, last plain, last cipher)
// that each byte advances.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace dice::hfe::net {

class GameSpyCrypt {
public:
	GameSpyCrypt(const uint8_t* key, size_t keySize);

	uint8_t decrypt(uint8_t cipher);
	uint8_t encrypt(uint8_t plain); // the inverse, for tests and servers
	void decrypt(uint8_t* data, size_t size);

private:
	uint8_t keystream(); // advances the cards; the byte to combine with
	uint8_t keyRandom(unsigned limit, const uint8_t* key, size_t keySize, uint8_t& sum, size_t& position);

	std::array<uint8_t, 256> m_cards {};
	uint8_t m_rotor = 0, m_ratchet = 0, m_avalanche = 0, m_lastPlain = 0, m_lastCipher = 0;
};

} // namespace dice::hfe::net
