/* Mock controller memory/CRC, exercise the actual pin/reset/RAM-download code. */
#define NV_PROBE_MAX 128u
static int nv_initialize_panel(void);
static uint8_t memory[0x800000];
static uint32_t gpios[0x6000 / 4], page, injection;
static unsigned engineering_resets, releases, boots, total_bytes;
static uint32_t read_le(uint32_t a, unsigned n)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint32_t)memory[a+i] << (i*8);
    return v;
}
static void store_le(uint32_t a, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) memory[a+i] = (uint8_t)(v >> (i*8)); }
static void nv_sleep(unsigned n) { (void)n; }
static uint32_t nv_read(vm_offset_t b, uint32_t a)
{ (void)b; assert(a < sizeof(gpios)); return gpios[a/4]; }
static void nv_write(vm_offset_t b, uint32_t a, uint32_t v)
{
    (void)b; assert(a < sizeof(gpios));
    if (a == 0x4004 && (v & 2)) { assert(engineering_resets == 1); releases++; }
    gpios[a/4] = v;
}
static int nv_transfer(const uint8_t *tx, uint8_t *rx, unsigned length)
{
    assert(length > 0 && length <= NV_PROBE_MAX);
    if (tx[0] == 0xff && length == 3 && !rx) {
        page = ((uint32_t)tx[1] << 15) | ((uint32_t)tx[2] << 7);
        return 0;
    }
    unsigned a = page | (tx[0] & 0x7f);
    assert(a + length < sizeof(memory));
    if (rx) { memset(rx, 0, 2); memcpy(rx + 2, memory + a, length - 2); return 0; }
    assert(tx[0] & 0x80);
    memcpy(memory + a, tx + 1, length - 1);
    if (a < 0x30000) total_bytes += length - 1;
    if (a == 0x7fff80) {
        assert(tx[1] == 0x5a && !(gpios[0x4004/4] & 2));
        engineering_resets++;
    }
    if (a == 0x3f0fe) assert(tx[1] == 0x69 && releases == 1);
    if (a == 0x3f10d) {
        assert(tx[1] == 1 && (memory[0x3f30e] & 0x80));
        assert(memory[0x22d60] == 0 && memory[0x22d50] == 0xae);
        for (unsigned i = 0; i < sizeof(nv_fw_parts)/sizeof(nv_fw_parts[0]); ++i) {
            const struct nv_fw_partition *p = &nv_fw_parts[i];
            assert(!memcmp(memory + p->sram, nv_firmware + p->image, p->size + 1));
        }
        assert(read_le(0x3f128, 3) == nv_fw_parts[0].sram);
        assert(read_le(0x3f12c, 3) == nv_fw_parts[1].sram);
        assert(read_le(0x3f118, 3) == nv_fw_parts[0].size);
        assert(read_le(0x3f130, 3) == nv_fw_parts[1].size);
        assert(read_le(0x3f100, 4) == nv_fw_parts[0].crc);
        assert(read_le(0x3f104, 4) == nv_fw_parts[1].crc);
        store_le(0x3f120, nv_fw_parts[0].crc ^ (injection == 1));
        store_le(0x3f124, nv_fw_parts[1].crc);
        memory[0x3f133] = injection == 2 ? 5 : 4;
        memory[0x22d60] = injection == 3 ? 0 : 0xa1;
        boots++;
    }
    return 0;
}
static void initialize_mock(unsigned fail)
{
    memset(memory, 0, sizeof(memory)); memset(gpios, 0, sizeof(gpios));
    memset(&nv, 0, sizeof(nv)); page = 0; injection = fail;
    engineering_resets = releases = boots = total_bytes = 0;
    const uint8_t id[6] = {0x0c,0,0,0x72,0x66,3};
    memcpy(memory + 0x1f64e, id, sizeof(id));
    if (fail == 4) memory[0x1f64e] = 0;
}
static void test_firmware(void)
{
    initialize_mock(0);
    assert(nv_initialize_panel() == 0 && engineering_resets == 1 && releases == 1 && boots == 1);
    assert(gpios[0x4000/4] == 0x3c3 && gpios[0x5000/4] == 0x1c1);
    assert(total_bytes > 97000); /* real pinned ILM/DLM/info bytes were all transferred */
    initialize_mock(1); assert(nv_initialize_panel() == EIO);
    initialize_mock(2); assert(nv_initialize_panel() == EIO);
    initialize_mock(3); assert(nv_initialize_panel() == ETIMEDOUT);
    initialize_mock(4); assert(nv_initialize_panel() == ENODEV && boots == 0);
    puts("PASS: pin/reset ordering, actual firmware partition writes, CRC banks/errors, reset timeout, chip guard");
}
