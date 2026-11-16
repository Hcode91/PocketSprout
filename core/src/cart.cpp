// PocketSprout - build a virtual Didj cartridge NAND from a FAT filesystem image.
//
// A Didj cartridge is a raw NAND chip formatted with UBI, holding a single dynamic volume
// ("ubi_Cartridge") whose contents are a FAT filesystem (mounted at /Cart). Cartridge dumps
// are often distributed as just that FAT image, so we wrap it in UBI on a virtual SLC chip
// (2 KB pages, 128 KB blocks, no ECC: the firmware treats our cart as an OTP-type cart).
#include "didj.h"
#include <cstdio>

namespace didj {

static u32 crc_table[256];
static void crc_init() {
    if (crc_table[1]) return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
// Linux crc32_le with seed 0xFFFFFFFF and no final inversion (what UBI uses)
static u32 ubi_crc(const u8* p, size_t n) {
    crc_init();
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void be32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void be64(u8* p, u64 v) { be32(p, (u32)(v >> 32)); be32(p + 4, (u32)v); }
static void be16(u8* p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

bool is_fat_image(const std::vector<u8>& d) {
    if (d.size() < 512) return false;
    if (d[510] != 0x55 || d[511] != 0xAA) return false;
    return (d[0] == 0xEB || d[0] == 0xE9) &&
           (!memcmp(&d[0x36], "FAT", 3) || !memcmp(&d[0x52], "FAT", 3));
}

bool build_cart_nand(const std::vector<u8>& fat, NandChip& chip, std::string* err) {
    const u32 PAGE = 2048, SPARE = 64, PPB = 64;
    const u32 PEB = PAGE * PPB;                 // 128 KB
    // The cart MTD has no sub-page support (ECC none), so UBI puts the VID header in page 1
    const u32 VID_OFF = 2048, DATA_OFF = 4096;
    const u32 LEB = PEB - DATA_OFF;             // 126976
    u32 need = (u32)((fat.size() + LEB - 1) / LEB);
    u32 blocks = 1024;                          // 128 MB
    while (blocks < (need + 2) * 102 / 100 + 8) blocks *= 2;
    if (blocks > 4096) { if (err) *err = "cartridge image too large"; return false; }

    chip.page_size = PAGE; chip.spare_size = SPARE; chip.pages_per_block = PPB;
    chip.num_pages = blocks * PPB;
    chip.data.assign((size_t)chip.num_pages * (PAGE + SPARE), 0xFF);
    chip.id[0] = 0x2C;
    chip.id[1] = blocks == 1024 ? 0xF1 : blocks == 2048 ? 0xDA : 0xDC;
    chip.id[2] = 0x00; chip.id[3] = 0x95; chip.id[4] = 0x40;
    chip.write_protect = false;

    auto page_ptr = [&](u32 peb, u32 page) -> u8* {
        return &chip.data[((size_t)peb * PPB + page) * (PAGE + SPARE)];
    };
    // Write bytes at an offset within a PEB (data area only, skipping spare)
    auto write_peb = [&](u32 peb, u32 off, const u8* src, u32 len) {
        while (len) {
            u32 pg = off / PAGE, po = off % PAGE;
            u32 n = PAGE - po; if (n > len) n = len;
            memcpy(page_ptr(peb, pg) + po, src, n);
            off += n; src += n; len -= n;
        }
    };

    u64 sqnum = 0;
    auto ec_hdr = [&](u32 peb) {
        u8 h[64]; memset(h, 0, sizeof h);
        be32(h, 0x55424923);                    // "UBI#"
        h[4] = 1;                               // version
        be64(h + 8, 1);                         // erase counter
        be32(h + 16, VID_OFF);
        be32(h + 20, DATA_OFF);
        be32(h + 60, ubi_crc(h, 60));
        write_peb(peb, 0, h, 64);
    };
    auto vid_hdr = [&](u32 peb, u32 vol_id, u32 lnum, u8 compat) {
        u8 h[64]; memset(h, 0, sizeof h);
        be32(h, 0x55424921);                    // "UBI!"
        h[4] = 1;                               // version
        h[5] = 1;                               // dynamic volume
        h[7] = compat;
        be32(h + 8, vol_id);
        be32(h + 12, lnum);
        be64(h + 40, ++sqnum);
        be32(h + 60, ubi_crc(h, 60));
        write_peb(peb, VID_OFF, h, 64);
    };

    for (u32 b = 0; b < blocks; b++) ec_hdr(b);

    // Volume table (layout volume, 2 copies)
    const u32 REC = 172, NREC = 128;
    std::vector<u8> vtbl(REC * NREC, 0);
    for (u32 i = 0; i < NREC; i++) {
        u8* r = &vtbl[i * REC];
        if (i == 0) {
            be32(r + 0, need);                  // reserved PEBs
            be32(r + 4, 1);                     // alignment
            be32(r + 8, 0);                     // data pad
            r[12] = 1;                          // dynamic
            r[13] = 0;                          // update marker
            const char* nm = "ubi_Cartridge";
            be16(r + 14, (u16)strlen(nm));
            memcpy(r + 16, nm, strlen(nm));
        }
        be32(r + 168, ubi_crc(r, 168));
    }
    for (u32 l = 0; l < 2; l++) {
        vid_hdr(l, 0x7FFFEFFF, l, 5);           // layout volume, compat = REJECT
        write_peb(l, DATA_OFF, vtbl.data(), (u32)vtbl.size());
    }
    // Data volume LEBs
    for (u32 l = 0; l < need; l++) {
        u32 peb = 2 + l;
        vid_hdr(peb, 0, l, 0);
        size_t off = (size_t)l * LEB;
        u32 n = (u32)std::min<size_t>(LEB, fat.size() - off);
        write_peb(peb, DATA_OFF, &fat[off], n);
    }
    chip.dirty = false;
    chip.reset_state();
    return true;
}

} // namespace didj
