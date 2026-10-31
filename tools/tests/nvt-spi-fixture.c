/* Bounded FIFO emulator for the actual nv_transfer function. */
static int nv_transfer(const uint8_t *, uint8_t *, unsigned);
static unsigned expected, written, shifted, consumed, queued, mode;
static uint8_t actual[128], expected_tx[128];
static bool duplex;
static void delay(unsigned n) { (void)n; }
static bool nv_clear(vm_offset_t b, uint32_t r, uint32_t m)
{ (void)b; (void)r; (void)m; return true; }
static void nv_abort(void) { nv.dead = true; }
static uint32_t nv_read(vm_offset_t b, uint32_t r)
{
    (void)b;
    switch (r) {
    case NV_STATUS: return 0;
    case NV_IRQ:
        if (mode == 1) return 2; /* controller error */
        if (mode == 2) return 0; /* bounded timeout */
        shifted = written;
        queued = 0;
        return shifted == expected ? NV_DONE : 0;
    case NV_TX_COUNT: return queued;
    case NV_RX_COUNT: {
        if (!duplex || consumed == shifted) return 0;
        unsigned left = shifted - consumed;
        uint32_t count = (left + 3) / 4;
        if (shifted == expected && left % 4) count |= 0x80000000u | ((left % 4) << 28);
        return count;
    }
    case NV_RX: {
        uint32_t word = 0;
        for (unsigned i = 0; i < 4 && consumed < shifted; ++i)
            word |= (uint32_t)((consumed++ * 7u + 3u) & 255u) << (8u * i);
        return word;
    }
    default: assert(0); return 0;
    }
}
static void nv_write(vm_offset_t b, uint32_t r, uint32_t v)
{
    (void)b;
    if (r == NV_TX_LEN) assert(v == expected);
    else if (r == NV_RX_LEN) { assert(v == expected); duplex = true; }
    else if (r == NV_TX) {
        assert(++queued <= nv.tx_depth);
        for (unsigned i = 0; i < 4 && written < expected; ++i)
            actual[written++] = (uint8_t)(v >> (8u * i));
    }
}
static void reset(unsigned length)
{
    memset(&nv, 0, sizeof(nv));
    nv.tx_depth = nv.rx_depth = 16;
    expected = length;
    written = shifted = consumed = queued = mode = 0;
    duplex = false;
    memset(actual, 0, sizeof(actual));
    for (unsigned i = 0; i < sizeof(expected_tx); ++i) expected_tx[i] = (uint8_t)(i * 11u);
}
static void test_transfer(void)
{
    uint8_t rx[128];
    const unsigned sizes[] = {1, 3, 4, 31, 32, 63, 64, 65, 66, 67, 127, 128};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        reset(sizes[i]);
        memset(rx, 0, sizeof(rx));
        assert(nv_transfer(expected_tx, rx, expected) == 0);
        assert(!memcmp(actual, expected_tx, expected));
        for (unsigned n = 0; n < expected; ++n) assert(rx[n] == (uint8_t)(n * 7u + 3u));
        reset(sizes[i]);
        assert(nv_transfer(expected_tx, NULL, expected) == 0);
        assert(!memcmp(actual, expected_tx, expected));
    }
    reset(67); mode = 1;
    assert(nv_transfer(expected_tx, rx, expected) == EIO && nv.dead);
    reset(67); mode = 2;
    assert(nv_transfer(expected_tx, rx, expected) == ETIMEDOUT && nv.dead);
    reset(67);
    assert(nv_transfer(expected_tx, rx, 129) == EINVAL && !nv.dead);
    puts("PASS: FIFO transfers across 64-byte boundary, partial words, errors, timeout");
}
