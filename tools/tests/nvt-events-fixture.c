/* Feed independent down/move/empty/malformed frames into the actual HID loop. */
static int nv_run_touch(void);
static unsigned position, event_count;
static bool fail_with_contact;
static struct { int slot, down, x, y; } events[12];
static int nv_initialize_panel(void) { return 0; }
static void nv_sleep(unsigned n) { (void)n; }
static uint32_t nv_read(vm_offset_t b, uint32_t r) { (void)b; (void)r; return 0; }
static void sweet_hid_touch(int slot, int down, int x, int y)
{
    assert(event_count < 12);
    events[event_count].slot = slot; events[event_count].down = down;
    events[event_count].x = x; events[event_count++].y = y;
}
static int nv_read_memory(uint32_t address, uint8_t *data, unsigned length)
{
    assert(address == 0x22d78 && length == 16);
    const uint8_t info[16] = {7,248,18,36,4,56,9,96};
    memcpy(data, info, sizeof(info));
    return 0;
}
static void contact(uint8_t *p, unsigned id, unsigned status, unsigned x, unsigned y)
{
    p[0] = (uint8_t)((id << 3) | status);
    p[1] = (uint8_t)(x >> 4); p[2] = (uint8_t)(y >> 4);
    p[3] = (uint8_t)(((x & 15) << 4) | (y & 15));
}
static int nv_register(uint32_t address, uint8_t *data, unsigned length)
{
    assert(address == 0x22d00 && length == NV_PACKET_BYTES);
    memset(data, 0xff, length);
    if (fail_with_contact) {
        if (position++ == 0) { contact(data, 1, 1, 100, 200); return 0; }
        return EIO;
    }
    switch (position++) {
    case 0: contact(data, 1, 1, 100, 200); return 0;
    case 1: case 2: contact(data, 1, 2, 150, 220); return 0;
    case 3: contact(data, 1, 2, 150, 220); contact(data + 6, 10, 1, 1079, 2399); return 0;
    case 4: return 0; /* all FF: release both, do not ignore as a watchdog */
    case 5: memset(data, 0x77, 6); return 0;
    case 6: contact(data, 1, 1, 1080, 200); return 0; /* invalid coordinate */
    case 7: contact(data, 0, 1, 100, 200); return 0; /* invalid slot */
    case 8: contact(data, 1, 1, 10, 20); contact(data + 6, 1, 1, 30, 40); return 0;
    case 9: contact(data, 1, 1, 50, 60); return 0; /* valid stream recovers */
    default: return EIO;
    }
}
static void test_events(void)
{
    struct nv_frame f = {0};
    uint8_t packet[NV_PACKET_BYTES] = {0};
    assert(nv_decode_packet(packet, 64, &f) == NV_FRAME_INVALID);
    assert(nv_decode_packet(packet, sizeof(packet), &f) == NV_FRAME_VALID && f.mask == 0);
    memset(packet, 0xfd, 6);
    assert(nv_decode_packet(packet, sizeof(packet), &f) == NV_FRAME_IGNORE);
    assert(nv_run_touch() == EIO);
    assert(event_count == 7);
    assert(events[0].slot == 0 && events[0].down && events[0].x == 100);
    assert(events[1].slot == 0 && events[1].down && events[1].x == 150);
    assert(events[2].slot == 9 && events[2].down && events[2].y == 2399);
    assert(events[3].slot == 0 && !events[3].down);
    assert(events[4].slot == 9 && !events[4].down);
    assert(events[5].slot == 0 && events[5].down && events[5].x == 50);
    assert(events[6].slot == 0 && !events[6].down); /* bus failure releases held finger */
    position = event_count = 0; fail_with_contact = true;
    assert(nv_run_touch() == EIO);
    assert(event_count == 2 && events[0].down && !events[1].down);
    puts("PASS: down/move/up, slot10, empty frames, malformed packets, failure releases");
}
