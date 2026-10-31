static void put32(uint8_t *p, uint32_t n) { memcpy(p, &n, 4); }

int main(void)
{
 struct msm_timespec when = { INT64_MAX, 999999999 };
 assert(millisecondsUntil(&when) == GPU_PATIENCE_MS);
 when.seconds = 10; when.nanoseconds = 1500000; assert(millisecondsUntil(&when) == 1);
 when.nanoseconds = 0; assert(millisecondsUntil(&when) == 0);
 when.seconds = -1; assert(millisecondsUntil(&when) == 0);
 when.seconds = 11; when.nanoseconds = -1; assert(millisecondsUntil(&when) == 0);
 when.nanoseconds = 1000000000; assert(millisecondsUntil(&when) == 0);
 uint32_t ring[CORE_RING_WORDS] = {0}, scratch[64] = {0};
 kept.ring_mem = (uintptr_t)ring; kept.scratch = (uintptr_t)scratch;
 kept_free_count = 1; kept_free[0].start = KEPT_VA_START; kept_free[0].size = KEPT_VA_SIZE;
 assert(sweet_gpu_address(0) == 0); assert(sweet_gpu_address(UINT64_MAX) == 0);
 uint64_t a = sweet_gpu_address(1), b = sweet_gpu_address(65537);
 assert(a == KEPT_VA_START && b == a + 65536);
 sweet_gpu_address_free(a, 1); sweet_gpu_address_free(b, 65537);
 assert(kept_free_count == 1 && kept_free[0].size == KEPT_VA_SIZE);
 uint64_t addresses[1] = {0x30000000}; uint32_t words[1] = {4};
 assert(sweet_gpu_submit(addresses, words, 1) == 1 && wptr == 15);
 assert(ring[5] == 0x30000000 && ring[7] == 4);
 assert(!sweet_gpu_done(1)); scratch[KEPT_FENCE_AT / 4] = 1; assert(sweet_gpu_done(1));
 kept.at = CORE_RING_WORDS - 3; kept.sent = kept.at; rptr = 100;
 assert(sweet_gpu_submit(addresses, words, 1) == 2 && wptr == 12);
 unsigned old = kept.at; rptr = (kept.at + 1) % CORE_RING_WORDS; waits = 0;
 assert(sweet_gpu_submit(addresses, words, 1) == 0 && waits == 20000 && kept.at == old);
 assert(sweet_gpu_submit(addresses, words, UINT32_MAX) == 0);
 uint8_t fw[64] = {0}; put32(fw + 4, 16); assert(picasso_gmu_valid(fw, 32));
 for (unsigned n = 0; n < 32; ++n) assert(!picasso_gmu_valid(fw, n));
 put32(fw + 4, UINT32_MAX); assert(!picasso_gmu_valid(fw, 32));
 put32(fw, 0x3ffc); put32(fw + 4, 16); assert(!picasso_gmu_valid(fw, 32));
 uint8_t mdt[180] = {0}; put32(mdt, 0x464c457f); mdt[4] = mdt[5] = 1;
 put32(mdt + 28, 52); mdt[42] = 32; mdt[44] = 3;
 put32(mdt + 84, 1); put32(mdt + 100, 32); put32(mdt + 108, 2u << 24);
 put32(mdt + 116, 1); put32(mdt + 132, 128); put32(mdt + 136, 256);
 assert(picasso_zap_valid(mdt, sizeof(mdt), 128, 0x2000));
 assert(!picasso_zap_valid(NULL, 0, 128, 0x2000));
 for (unsigned n = 0; n < 180; ++n) assert(!picasso_zap_valid(mdt, n, 128, 0x2000));
 put32(mdt + 136, 0x2001); assert(!picasso_zap_valid(mdt, 180, 128, 0x2000));
 uint32_t rng = 0x7250;
 for (unsigned i = 0; i < 30000; ++i) {
  for (unsigned k = 0; k < 64; ++k) { rng = rng * 1664525 + 1013904223; fw[k] = (uint8_t)(rng >> 24); }
  (void)picasso_gmu_valid(fw, i % 65);
  (void)picasso_zap_valid(fw, i % 65, 128, 0x2000);
 }
 puts("PASS actual allocator, ring wrap/full, fences, malformed/fuzzed firmware (ASan/UBSan)");
 return 0;
}
