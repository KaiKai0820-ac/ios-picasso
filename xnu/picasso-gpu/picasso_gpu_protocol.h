/* Bounds checks shared by the kernel firmware loader and host regression tests. */
#ifndef PICASSO_GPU_PROTOCOL_H
#define PICASSO_GPU_PROTOCOL_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static inline uint32_t
picasso_le32(const void *ptr)
{
	const uint8_t *p = ptr;
	return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline bool
picasso_gmu_valid(const uint8_t *data, uint32_t bytes)
{
	uint32_t at = 0, payloads = 0;
	if (!data || bytes < 32 || bytes > 4u * 1024u * 1024u) return false;
	while (at < bytes) {
		if (bytes - at < 16) return false;
		uint32_t address = picasso_le32(data + at), size = picasso_le32(data + at + 4);
		uint32_t type = picasso_le32(data + at + 8), value = picasso_le32(data + at + 12);
		at += 16;
		if (size > bytes - at || (size & 3u) || (address & 3u)) return false;
		if (size) {
			uint32_t limit;
			if (address < 0x4000u) limit = 0x4000u;
			else if (address >= 0x10004000u && address < 0x10008000u) limit = 0x10008000u;
			else if (address >= 0x4000u && address < 0x80000000u) limit = 0x80000000u;
			else return false;
			if (size > limit - address) return false;
			++payloads;
		} else if (type == 1 || type == 7) {
			if (!value || value > 0x1000000u || (address & 4095u) ||
			    address >= 0x80000000u || value > 0x80000000u - address) return false;
		}
		at += size;
	}
	return at == bytes && payloads != 0;
}

/* Qualcomm split MDT: header + phdrs + hash bytes, one non-hash PT_LOAD.
 * Segment physical addresses can be relocatable; PAS uses the board carveout. */
static inline bool
picasso_zap_valid(const uint8_t *mdt, uint32_t bytes, uint32_t segment_bytes, uint32_t reservation)
{
	if (!mdt || bytes < 116 || !segment_bytes || segment_bytes > reservation ||
	    picasso_le32(mdt) != 0x464c457fu || mdt[4] != 1 || mdt[5] != 1 ||
	    picasso_le32(mdt + 28) != 52 || mdt[42] != 32 || mdt[43] != 0) return false;
	uint32_t count = mdt[44] | ((uint32_t)mdt[45] << 8);
	if (count < 3 || count > 32 || 52u + count * 32u > bytes) return false;
	uint32_t hashes = picasso_le32(mdt + 100), loadable = 0;
	if (hashes != bytes - (52u + count * 32u)) return false;
	for (uint32_t i = 0; i < count; ++i) {
		const uint8_t *p = mdt + 52u + i * 32u;
		if (picasso_le32(p) != 1 || ((picasso_le32(p + 24) >> 24) & 7u) == 2) continue;
		uint32_t filesz = picasso_le32(p + 16), memsz = picasso_le32(p + 20);
		if (filesz != segment_bytes || memsz < filesz || memsz > reservation) return false;
		++loadable;
	}
	return loadable == 1;
}
#endif
