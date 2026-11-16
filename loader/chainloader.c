/*
 * chainloader.c -- Warm-takeover kexec launcher application for iPod nano 7G
 * (spec: docs/tasks/piece3-handoff.md, plan: docs/research/dualboot-chainloader-plan.md).
 *
 * Sequence:
 *   1. Locate and stage payload.bin in DRAM:
 *      - Checks filesystem paths (/Apps/DualBoot/payload.bin, etc.)
 *      - Reads payload into contiguous DRAM staging buffer
 *      - Falls back to embedded default_payload_bin if no external file found
 *   2. Orders hardware quiescence via hw_shutdown_all():
 *      - Display pipeline (commits stopped, TE interrupt masked)
 *      - FMC / NAND controller (settled, unselected, IRQs masked)
 *      - DMA engines 1 & 2 (channels drained and aborted)
 *      - Timers (all 8 channels stopped and interrupts cleared)
 *      - VIC0 & VIC1 (all interrupt lines masked)
 *   3. Hands over CPU via handoff_to_payload():
 *      - Copies position-independent takeover trampoline to SRAM 0x22000000
 *      - Hooks supervisor table slot 0 at 0x08917714
 *      - Invokes svc #70 to enter SVC mode
 *      - Trampoline disables MMU, flushes/invalidates caches, moves payload
 *        to physical DRAM 0x08000000, and jumps to 0x08000000.
 */

#include <stdint.h>
#include <stdbool.h>
#include "shutdown.h"
#include "handoff.h"
#include "report.h"
#include "checkpoint_report.h"
#include "btoc_report.h"

#if defined(__has_include)
  #if __has_include("hb_sdk.h")
    #include "hb_sdk.h"
  #endif
#endif

/* Forward declarations in case hb_sdk.h is not in include path */
#ifndef HB_SDK_H
uint32_t hb_fs_size(const char *path);
uint32_t hb_fs_read(const char *path, void *buf, uint32_t max_size);
bool hb_fs_write(const char *path, const void *data, uint32_t size);
bool hb_fs_mkdir(const char *path);
#endif

/* Staging buffer in high DRAM for payload relocation */
#define STAGING_MAX_BYTES (1024u * 1024u)   /* 1 MiB staging capacity */
static uint8_t s_staging_buffer[STAGING_MAX_BYTES] __attribute__((aligned(32)));

/* Candidate paths searched for bare-metal payload */
static const char * const s_payload_search_paths[] = {
    "/Apps/DualBoot/payload.bin",
    "/Apps/Chainloader/payload.bin",
    "/Apps/Startup/payload.bin",
    "payload.bin"
};

#define NUM_SEARCH_PATHS (sizeof(s_payload_search_paths) / sizeof(s_payload_search_paths[0]))

/*
 * chainloader_main -- core execution routine:
 * loads payload, shuts down hardware, and transfers execution.
 */
/* export the previous NanoOS run's report (RAM mirror, report.h) to /Apps/Data/NanoOS/report.txt */
/* Optional read-only native checkpoint audit. Validate/copy into our own
 * staging BSS before any filesystem calls can allocate over packet RAM. */
static void export_checkpoint(void)
{
    struct cp_packet_header header;
    int found = 0;
    for (unsigned i = 0; i < RPT_MIRRORS; ++i) {
        uintptr_t base = cp_packet_base(i);
        if (cp_packet_copy((const volatile struct cp_packet_header *)(uintptr_t)base,
                           (const volatile uint8_t *)(uintptr_t)(base + RPT_DATA),
                           s_staging_buffer, sizeof s_staging_buffer, &header)) {
            found = 1;
            break;
        }
    }
    if (!found) return;
    hb_fs_mkdir("/Apps/Data/NanoOS");
    if (!hb_fs_write("/Apps/Data/NanoOS/checkpoint.bin", s_staging_buffer, CP_DATA_BYTES)) return;
    if (!hb_fs_write("/Apps/Data/NanoOS/checkpoint-records.bin", s_staging_buffer + CP_DATA_BYTES, CP_RECORD_BYTES)) return;
    if (!hb_fs_write("/Apps/Data/NanoOS/checkpoint-header.bin", &header, sizeof header)) return;
    cp_packet_clear_matching(&header);
}

static void export_btoc(void)
{
    struct bt_packet_header header;
    int found = 0;
    for (unsigned i = 0; i < RPT_MIRRORS; ++i) {
        uintptr_t base = bt_packet_base(i);
        if (bt_packet_copy((const volatile struct bt_packet_header *)base,
                           (const volatile uint8_t *)(base + RPT_DATA),
                           s_staging_buffer, sizeof s_staging_buffer, &header)) {
            found = 1;
            break;
        }
    }
    if (!found) return;
    hb_fs_mkdir("/Apps/Data/NanoOS");
    if (!hb_fs_write("/Apps/Data/NanoOS/btoc.bin", s_staging_buffer, BT_DATA_BYTES)) return;
    if (!hb_fs_write("/Apps/Data/NanoOS/btoc-records.bin", s_staging_buffer + BT_DATA_BYTES, BT_RECORD_BYTES)) return;
    if (!hb_fs_write("/Apps/Data/NanoOS/btoc-header.bin", &header, sizeof header)) return;
    bt_packet_clear_matching(&header);
}

static void export_report(void)
{
    uint32_t base = rpt_mirror_find();
    volatile struct rpt_hdr *h = (volatile struct rpt_hdr *)(uintptr_t)base;
    uint32_t n = base ? h->len[RPT_M8_SLOT] : 0;
    static char copy[RPT_MIR_SIZE];
    for (uint32_t i = 0; i < n; i++) copy[i] = ((volatile char *)(base + RPT_DATA))[i];
    /* Preserve text before exporting the larger packet can allocate RAM. */
    export_checkpoint();
    export_btoc();
    if (!base) return;
    hb_fs_mkdir("/Apps/Data/NanoOS");
    if (hb_fs_write("/Apps/Data/NanoOS/report.txt", copy, n)) rpt_mirror_clear();
}


void chainloader_main(void)
{
    export_report();
    const void *payload_ptr = default_payload_bin;
    uint32_t payload_len = DEFAULT_PAYLOAD_SIZE;

    /* 1. Attempt to load payload from filesystem */
    for (uint32_t i = 0; i < NUM_SEARCH_PATHS; i++) {
        const char *path = s_payload_search_paths[i];
        uint32_t file_len = hb_fs_size(path);

        if (file_len > 0 && file_len <= sizeof(s_staging_buffer)) {
            uint32_t bytes_read = hb_fs_read(path, s_staging_buffer, file_len);
            if (bytes_read == file_len) {
                payload_ptr = s_staging_buffer;
                payload_len = bytes_read;
                break;
            }
        }
    }

    /* 2. Orderly hardware shutdown (Display, FMC, DMA, Timers, VIC) */
    hw_shutdown_all();

    /* 3. SRAM takeover and payload branch (never returns) */
    handoff_to_payload(payload_ptr, payload_len);
}

/*
 * Entry point for relocatable .hbapp bundles (HBAppKind / mkrelocapp)
 */
__attribute__((section(".text.entry"), used, noinline))
void payload_entry(void)
{
    chainloader_main();
}

/*
 * Standard main entry point for standalone ELF/bin builds
 */
int main(void)
{
    chainloader_main();
    return 0;
}

/*
 * Raw surface entry points for compatibility with hb_raw_surface runtime
 */
void hb_raw_init(void)
{
    chainloader_main();
}

void hb_raw_frame(uint32_t *fb __attribute__((unused)))
{
    /* Should never be reached as chainloader_main does not return */
}
