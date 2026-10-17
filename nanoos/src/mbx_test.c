#include "mbx.h"
#include "lcd.h"
#include "fmt.h"
#include "input.h"
#include "timer.h"
#include "report.h"
/* DevMode "GPU" page, round-5 case cycler: Vol+/Vol- select R50..R53, each
 * selection re-runs mbx_init() from cold and renders ONE frame into a
 * green-filled portrait 240x432 target (magenta ISP background) from the
 * RetailOS gltest frame-11 segments S0..S2 (docs/research/
 * mbx-gltest-frame.txt, words literal) and the NanoOS Gouraud triangle,
 * after the first-launch startup prefix (now the default mbx.c path, so
 * R50..R52 run the same sequence), or replaying scene 1.
 * Around the frame every 4 KB page of the GPU's 32 MB window is summed
 * before and after to find where the GPU wrote. Results cycle through ten
 * pages, then the raw aux dump (see the round-3/4 build reports). */
#define W 240            /* portrait render target */
#define H 432
#define LW LCD_WIDTH     /* landscape LCD, 432x240 */
#define LH LCD_HEIGHT
/* RetailOS command format: header 0x10000073, ISP (ONE/ZERO blend,
 * Gouraud, depth always), alpha, region, format 0x503 (x y z + packed
 * 0xAABBGGRR colour), one unknown zero word, primitive, control, then
 * four words per vertex. */
#define HDR 0x10000073u
#define ISP 0x22200e00u
#define ALPHA 0xe0000000u
#define REGION 0x2003801au
#define FMT 0x503u
#define LIST 0x70000000u
#define CTL 0xb00b0800u
struct vtx { union { float f; uint32_t u; } x; float y, z; uint32_t abgr; };
struct seg { uint32_t hdr, isp, alpha, region, fmt, zero, prim, ctl; struct vtx v[3]; };
static const struct seg tri = { HDR, ISP, ALPHA, REGION, FMT, 0, LIST, CTL, {
    {{0}, 30, 1, 0xff0000ffu}, {{210}, 45, 1, 0xff00ff00u},
    {{210}, 385, 1, 0xffff0000u} } };
/* gltest frame 11, segments between the FIFO2 doorbells: S0 prefix
 * (primitive 0x70020000), S1 colour clear, S2 depth clear, S3 VGP
 * programs/constants and the textured quad (opaque constants kept) */
static const uint32_t s0[20] = {
    0x10000073u, 0x22200e00u, 0xe0000000u, 0x2003801au, 0x00000503u, 0x00000000u,
    0x70020000u, 0xb00b0800u, 0xc0000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xc0000000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0xbf800000u, 0x00000000u,
    0x00000000u, 0x00000000u,
};
static const uint32_t s1[20] = {
    0x10000073u, 0x22200e00u, 0xe0000000u, 0x2003801au, 0x00000503u, 0x00000000u,
    0x70000000u, 0xb00b0800u, 0x00000000u, 0x00000000u, 0x3f800000u, 0xff40140cu,
    0x43f00000u, 0x00000000u, 0x3f800000u, 0xff40140cu, 0x00000000u, 0x44580000u,
    0x3f800000u, 0xff40140cu,
};
static const uint32_t s2[17] = {
    0x10000073u, 0x22207f80u, 0xe0000000u, 0x2003801au, 0x00000003u, 0x00000000u,
    0x40000000u, 0xb00b0800u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x43f00000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x44580000u, 0x3f800000u,
};
static const uint32_t s3[381] = {
    0x80000004u, 0x05000700u, 0x90000007u, 0xe4e4e408u, 0x4c440000u, 0xe4e4e408u,
    0x4c840010u, 0xe4e4e408u, 0x4d040020u, 0xe4e4e408u, 0x4e040030u, 0xe455c604u,
    0x07c45042u, 0xe4e4e401u, 0x03c20003u, 0xe4e4e408u, 0x14461003u, 0xe4e4e408u,
    0x14861013u, 0x80000008u, 0x00000c08u, 0x9008000cu, 0xe4e4e408u, 0x410400a0u,
    0xe4e4e408u, 0x40440080u, 0xe4e4e408u, 0x40840090u, 0xe4e4e408u, 0x420400b0u,
    0xe4aaaa4au, 0x67ca0000u, 0x80000010u, 0x0660180du, 0x900d0018u, 0xe4e4aa01u,
    0x53c10004u, 0xe400e40au, 0x57c60044u, 0xe455e409u, 0x57c60045u, 0xe4e4ff04u,
    0x57ca0053u, 0xe400e404u, 0x4bc60055u, 0xe455e404u, 0x57c60055u, 0xe400ff22u,
    0x53c10073u, 0xe4e4e404u, 0x4bca0042u, 0xe455ff22u, 0x53c10073u, 0xe4e4e404u,
    0x57ca0045u, 0xe400e402u, 0x004a0032u, 0xe455e402u, 0x008a0035u, 0x80000020u,
    0x06501e1eu, 0x901e001eu, 0xe4e4e408u, 0x150600d0u, 0x80000040u, 0x0500291fu,
    0x901f0029u, 0x5500e403u, 0x57d45cc1u, 0xe4e4e412u, 0x47c2000cu, 0xe4e4e401u,
    0x6bc11006u, 0xe4e4e401u, 0x6fc11007u, 0xe4e4e401u, 0x4bc11004u, 0xe4e4e405u,
    0x77c20000u, 0xe4e4e407u, 0x444600c5u, 0xe4e4e407u, 0x448600d5u, 0xe4e4e407u,
    0x450600e5u, 0xe4e4e404u, 0x43ca000du, 0xe4e4e401u, 0x77c11008u, 0x80000080u,
    0x00803533u, 0x90330035u, 0xe4e4e407u, 0x57ca0011u, 0xe4e4e406u, 0x57c20005u,
    0xe4e4e404u, 0x47ca0051u, 0x80000100u, 0x05103c36u, 0x9036003cu, 0xe4e4e402u,
    0x73c6404cu, 0xe4e4e404u, 0x57c680ebu, 0xe4e4e407u, 0x5bc680c1u, 0xe4e4e403u,
    0x4be682dau, 0xe4e4e412u, 0x47c2000cu, 0xe400e40au, 0x5bc64036u, 0xe4e4e403u,
    0x4bea0256u, 0x80000200u, 0x05105141u, 0x90410051u, 0xe4e4e402u, 0x73c6404cu,
    0xe4e4e442u, 0x57c9800bu, 0xe4e4e407u, 0x5fca0055u, 0xe4e4e40au, 0x5fc65067u,
    0xe4e4e406u, 0x5bc20007u, 0xe4e4e410u, 0x5fca0067u, 0xe4e4e404u, 0x5bca0065u,
    0xe4e4e407u, 0x5fc69017u, 0xe4e4e407u, 0x63ca0061u, 0xe4e4e405u, 0x5fc20007u,
    0xe400e40au, 0x5bc64038u, 0xe4e4e404u, 0x57ca007au, 0xe4e4e403u, 0x4be682d5u,
    0xe4e4e404u, 0x57ca007bu, 0xe4e4e404u, 0x57c680e5u, 0xe4e4e412u, 0x47c2000cu,
    0xe4e4e403u, 0x4bea0256u, 0x80000400u, 0x01106363u, 0x90630063u, 0xe4e4c601u,
    0x07c10004u, 0x80000800u, 0x05506865u, 0x90650068u, 0xe4e4e401u, 0x03c00000u,
    0xe455c604u, 0x07c40042u, 0xe4e4e401u, 0x0fc00003u, 0xe4e4e401u, 0x13c00004u,
    0x80001000u, 0x00806969u, 0x90690069u, 0xe4e4e401u, 0x0fc00003u, 0x80002000u,
    0x00806f6eu, 0x906e006fu, 0xe4e4ff05u, 0x57c00004u, 0xe4e4e404u, 0x13c20045u,
    0x80004000u, 0x05507473u, 0x90730074u, 0xe4e4aa04u, 0x57c9009bu, 0xe4e4e433u,
    0x0a020005u, 0xa0100011u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0xbf800000u, 0x3f800000u, 0xa0530054u, 0x00000000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x40e00000u, 0x3f808084u, 0x40e00000u,
    0x40e00000u, 0xa0660066u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xa0000003u, 0x40559f87u, 0xbfac9e65u, 0x00000000u, 0x00000000u, 0x3f261a39u,
    0x3fcd8f18u, 0x3f800000u, 0x00000000u, 0x3e6a6ba5u, 0x3f110d82u, 0xbf877c1fu,
    0x40038e38u, 0x3e3fcc70u, 0x3eed5becu, 0xbf5db3d6u, 0x40600000u, 0xa00f000fu,
    0x3f808084u, 0x3f808084u, 0x3f808084u, 0x3f808084u, 0xa05d0062u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x080d84ddu, 0x00000000u, 0x00000000u,
    0x00000000u, 0x000000f0u, 0x000001b0u, 0xff40140cu, 0x3f800000u, 0x00000000u,
    0x093d4b28u, 0x0000000cu, 0x093d4b28u, 0x0000000cu, 0x00000003u, 0x00001406u,
    0x00000000u, 0x00000003u, 0x00000002u, 0x00000002u, 0x00000002u, 0xa0650065u,
    0x3c088889u, 0xbb97b426u, 0x3f808084u, 0x3f808084u, 0xa0630063u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0x3f000000u, 0xa0640064u, 0x00000000u, 0x42000000u,
    0x3f800000u, 0x3f808084u, 0xa0550056u, 0x3f800000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0xa0570057u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0xa0580059u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u,
    0x00000000u, 0xa05a005au, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u,
    0xa016001au, 0x3e4ccccdu, 0x3e4ccccdu, 0x3e4ccccdu, 0x3f800000u, 0x3f4ccccdu,
    0x3f4ccccdu, 0x3f4ccccdu, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0xa0120013u, 0x3f800000u, 0x3f800000u,
    0x3f800000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xa0140014u, 0x3e4ccccdu, 0x3e4ccccdu, 0x3e4ccccdu, 0x3f800000u, 0xa0140014u,
    0x3d247b8bu, 0x3d247b8bu, 0x3d247b8bu, 0x3f808084u, 0xa05b005bu, 0x3f800000u,
    0x3f800000u, 0x3fb8aa3bu, 0x3f99be61u, 0xa05c005cu, 0x3f800000u, 0x00000000u,
    0x3fb8aa3bu, 0x3f99be61u, 0x10000063u, 0x22620200u, 0xe0000000u, 0x00002503u,
    0x00000000u, 0x40020000u, 0xf0021004u, 0xbf800000u, 0xbf800000u, 0x00000000u,
    0xff4040ffu, 0x00000000u, 0x00000000u, 0x3f800000u, 0xbf800000u, 0x00000000u,
    0xff40ff40u, 0x3f800000u, 0x00000000u, 0xbf800000u, 0x3f800000u, 0x00000000u,
    0xffff4040u, 0x00000000u, 0x3f800000u, 0x3f800000u, 0x3f800000u, 0x00000000u,
    0xffffffffu, 0x3f800000u, 0x3f800000u,
};
/* same trace #298..#691: the scene-1 draw segment (pointer-looking
 * constants literal), doorbell after it */
static const uint32_t d1[394] = {
    0x80000004u, 0x05000700u, 0x90000007u, 0xe4e4e408u, 0x4c440000u, 0xe4e4e408u,
    0x4c840010u, 0xe4e4e408u, 0x4d040020u, 0xe4e4e408u, 0x4e040030u, 0xe455c604u,
    0x07c45042u, 0xe4e4e401u, 0x03c20003u, 0xe4e4e408u, 0x14461003u, 0xe4e4e408u,
    0x14861013u, 0x80000008u, 0x00000c08u, 0x9008000cu, 0xe4e4e408u, 0x410400a0u,
    0xe4e4e408u, 0x40440080u, 0xe4e4e408u, 0x40840090u, 0xe4e4e408u, 0x420400b0u,
    0xe4aaaa4au, 0x67ca0000u, 0x80000010u, 0x0660180du, 0x900d0018u, 0xe4e4aa01u,
    0x53c10004u, 0xe400e40au, 0x57c60044u, 0xe455e409u, 0x57c60045u, 0xe4e4ff04u,
    0x57ca0053u, 0xe400e404u, 0x4bc60055u, 0xe455e404u, 0x57c60055u, 0xe400ff22u,
    0x53c10073u, 0xe4e4e404u, 0x4bca0042u, 0xe455ff22u, 0x53c10073u, 0xe4e4e404u,
    0x57ca0045u, 0xe400e402u, 0x004a0032u, 0xe455e402u, 0x008a0035u, 0x80000020u,
    0x06501e1eu, 0x901e001eu, 0xe4e4e408u, 0x150600d0u, 0x80000040u, 0x0500291fu,
    0x901f0029u, 0x5500e403u, 0x57d45cc1u, 0xe4e4e412u, 0x47c2000cu, 0xe4e4e401u,
    0x6bc11006u, 0xe4e4e401u, 0x6fc11007u, 0xe4e4e401u, 0x4bc11004u, 0xe4e4e405u,
    0x77c20000u, 0xe4e4e407u, 0x444600c5u, 0xe4e4e407u, 0x448600d5u, 0xe4e4e407u,
    0x450600e5u, 0xe4e4e404u, 0x43ca000du, 0xe4e4e401u, 0x77c11008u, 0x80000080u,
    0x00803533u, 0x90330035u, 0xe4e4e407u, 0x57ca0011u, 0xe4e4e406u, 0x57c20005u,
    0xe4e4e404u, 0x47ca0051u, 0x80000100u, 0x05103c36u, 0x9036003cu, 0xe4e4e402u,
    0x73c6404cu, 0xe4e4e404u, 0x57c680ebu, 0xe4e4e407u, 0x5bc680c1u, 0xe4e4e403u,
    0x4be682dau, 0xe4e4e412u, 0x47c2000cu, 0xe400e40au, 0x5bc64036u, 0xe4e4e403u,
    0x4bea0256u, 0x80000200u, 0x05105141u, 0x90410051u, 0xe4e4e402u, 0x73c6404cu,
    0xe4e4e442u, 0x57c9800bu, 0xe4e4e407u, 0x5fca0055u, 0xe4e4e40au, 0x5fc65067u,
    0xe4e4e406u, 0x5bc20007u, 0xe4e4e410u, 0x5fca0067u, 0xe4e4e404u, 0x5bca0065u,
    0xe4e4e407u, 0x5fc69017u, 0xe4e4e407u, 0x63ca0061u, 0xe4e4e405u, 0x5fc20007u,
    0xe400e40au, 0x5bc64038u, 0xe4e4e404u, 0x57ca007au, 0xe4e4e403u, 0x4be682d5u,
    0xe4e4e404u, 0x57ca007bu, 0xe4e4e404u, 0x57c680e5u, 0xe4e4e412u, 0x47c2000cu,
    0xe4e4e403u, 0x4bea0256u, 0x80000400u, 0x01106363u, 0x90630063u, 0xe4e4c601u,
    0x07c10004u, 0x80000800u, 0x05506865u, 0x90650068u, 0xe4e4e401u, 0x03c00000u,
    0xe455c604u, 0x07c40042u, 0xe4e4e401u, 0x0fc00003u, 0xe4e4e401u, 0x13c00004u,
    0x80001000u, 0x00806969u, 0x90690069u, 0xe4e4e401u, 0x0fc00003u, 0x80002000u,
    0x00806f6eu, 0x906e006fu, 0xe4e4ff05u, 0x57c00004u, 0xe4e4e404u, 0x13c20045u,
    0x80004000u, 0x05507473u, 0x90730074u, 0xe4e4aa04u, 0x57c9009bu, 0xe4e4e433u,
    0x0a020005u, 0xa0100011u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0xbf800000u, 0x3f800000u, 0xa0530054u, 0x00000000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x40e00000u, 0x3f808084u, 0x40e00000u,
    0x40e00000u, 0xa0660066u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xa0000003u, 0x40664278u, 0xbe00a74cu, 0x00000000u, 0x00000000u, 0x3d779800u,
    0x3fdd9143u, 0x3f800000u, 0x00000000u, 0x3caeb6f2u, 0x3f1c5961u, 0xbf877c1fu,
    0x40038e38u, 0x3c8ef2c6u, 0x3effd814u, 0xbf5db3d6u, 0x40600000u, 0xa00f000fu,
    0x3f808084u, 0x3f808084u, 0x3f808084u, 0x3f808084u, 0xa05d0062u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x080d84ddu, 0x00000000u, 0x00000000u,
    0x00000000u, 0x000000f0u, 0x000001b0u, 0xff40140cu, 0x3f800000u, 0x00000000u,
    0x093d4b28u, 0x0000000cu, 0x093d4b28u, 0x0000000cu, 0x00000003u, 0x00001406u,
    0x00000000u, 0x00000003u, 0x00000002u, 0x00000002u, 0x00000002u, 0xa0650065u,
    0x3c088889u, 0xbb97b426u, 0x3f808084u, 0x3f808084u, 0xa0630063u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0x3f000000u, 0xa0640064u, 0x00000000u, 0x42000000u,
    0x3f800000u, 0x3f808084u, 0xa0550056u, 0x3f800000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0xa0570057u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0xa0580059u, 0x3f800000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u,
    0x00000000u, 0xa05a005au, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u,
    0xa016001au, 0x3e4ccccdu, 0x3e4ccccdu, 0x3e4ccccdu, 0x3f800000u, 0x3f4ccccdu,
    0x3f4ccccdu, 0x3f4ccccdu, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0xa0120013u, 0x3f800000u, 0x3f800000u,
    0x3f800000u, 0x3f800000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0xa0140014u, 0x3e4ccccdu, 0x3e4ccccdu, 0x3e4ccccdu, 0x3f800000u, 0xa0140014u,
    0x3d247b8bu, 0x3d247b8bu, 0x3d247b8bu, 0x3f808084u, 0xa05b005bu, 0x3f800000u,
    0x3f800000u, 0x3fb8aa3bu, 0x3f99be61u, 0xa05c005cu, 0x3f800000u, 0x00000000u,
    0x3fb8aa3bu, 0x3f99be61u, 0x100001efu, 0x22620200u, 0xe0000000u, 0x03319600u,
    0x12005ba0u, 0x86084610u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00002503u,
    0x00000000u, 0x42f00000u, 0x42f00000u, 0x43580000u, 0xc3580000u, 0x3f000000u,
    0x3f000000u, 0x3727c5acu, 0x40020000u, 0xf0021004u, 0xbf800000u, 0xbf800000u,
    0x00000000u, 0xff4040ffu, 0x00000000u, 0x00000000u, 0x3f800000u, 0xbf800000u,
    0x00000000u, 0xff40ff40u, 0x3f800000u, 0x00000000u, 0xbf800000u, 0x3f800000u,
    0x00000000u, 0xffff4040u, 0x00000000u, 0x3f800000u, 0x3f800000u, 0x3f800000u,
    0x00000000u, 0xffffffffu, 0x3f800000u, 0x3f800000u,
};
#define G_S0 1u
#define G_S1 2u
#define G_S2 4u
#define G_S3 8u
#define G_TRI 16u
#define G_D1 32u
#define SEG_P 6u         /* the prefix, recorded by the first mbx_begin() */
static const uint32_t *const seg_src[7] = { s0, s1, s2, s3, &tri.hdr, d1, mbx_prefix };
static const uint16_t seg_len[7] = { 20, 20, 17, 381, sizeof tri / 4, 394, 210 };
static const uint8_t seg_ck[7] = { MBX_CK_S0, MBX_CK_S1, MBX_CK_S2, MBX_CK_S3, MBX_CK_TRI,
                                   MBX_CK_S3, MBX_CK_PFX };
static const uint32_t seg_crc[7] = { 0x9439e7ddu, 0x0c911ea3u, 0x56162c96u, 0x361a9d19u, 0,
                                     0x43677d63u, 0x59d21f20u };
static const char seg_name[7][3] = { "S0", "S1", "S2", "S3", "TR", "D1", "P" };
#define NVAR 4u
#define R5 (G_S0 | G_S1 | G_S2 | G_TRI)
#define R5F (MBX_V_85C | MBX_V_NOBELL | MBX_V_CTX)
static const struct { uint8_t id, segs; uint16_t flags; char desc[24]; } var[NVAR] = {
    { 50, R5, R5F, "BASELINE" },
    { 51, R5, R5F | MBX_V_PFX, "PREFIX" },
    { 52, R5, R5F | MBX_V_PFX | MBX_V_860, "PREFIX+860FIX" },
    { 53, G_S0 | G_S1 | G_S2 | G_D1, R5F | MBX_V_PFX, "scene1 replay" },
};
/* render target: free DRAM above the LCD buffers (BUF1 ends 0x082e5400),
 * outside the kalloc heap, inside the GPU's 32 MB. The trace ring
 * (trace.h) shares its first 8 KB, so it is refilled just before the frame. */
#define target ((uint32_t *)0x08300000u)
#define SCAN_BASE 0x08000000u
#define SCAN_PAGES 8192u
extern uint8_t __data_start[], _stack_top[];
/* the suspected written tile: x 232..239, y 416..431 */
#define TX 232
#define TY 416
#define TILE_W 8
#define TILE_H 16
#define AUXW 4096u
/* page sums; after the second scan, scratch for sorting the aux copy */
static uint32_t sums[SCAN_PAGES];
static uint32_t variant, pending = 1, t_done;
static int ok, rc;
/* tgt (after the ISP): changed words, magenta words, first changed
 * index/value, bounding box of changed words, changed words inside/outside
 * the tile, tile words == 0x00bdd6ff, raw first two tile rows;
 * n_gpu/n_cpu: changed pages outside / inside CPU-written memory */
static uint32_t tgt_chg, tgt_mag, tgt_idx, tgt_val, bx0, by0, bx1, by1,
                t_in, t_out, t_bdd, raw[16], n_gpu, n_cpu, chg[16];
/* aux A statistics (indices/counts decimal): nonzero, distinct, min, max,
 * CRC32, most frequent value and its count, words == 0xffffffff, words /
 * low16 / high16 in [first, last]; constant-delta runs (first 20) */
static uint32_t a_nz, a_dist, a_min, a_max, a_hash, a_mode, a_mode_n, a_ff,
                a_raw, a_lo, a_hi, runs, run_i[20], run_n[20], run_v[20], run_d[20];
/* aux T/I against the previous stage: CRC32, first 8 (index, old, new) */
static uint32_t x_hash[2], x_n[2], x_i[2][8], x_o[2][8], x_v[2][8];
/* per recorded segment: source index, CRC32 of the source / stored words */
static uint32_t s_idx[MBX_SEGS], s_crc[MBX_SEGS], s_wcrc[MBX_SEGS];
#define NPAGES 10u
#define RAW_LINES 20u
#define RAW_PAGE (4u * RAW_LINES)
#define RAW_PER 52u      /* pages of RAW_PAGE words per aux copy */
static uint32_t raw_stage[3], n_stage, page_no, page_total;
static const char ev_site[8][4] = { "rst", "ack", "824", "828", "82c", "838", "83c", "6d8" };
static const char lbl[52][4] = {
    "rst", "ack", "@24", "@28", "@2c", "@38", "@3c", "@d8",
    "824", "828", "82c", "838", "83c", "6d8", "f24", "f28", "f2c", "f38", "f3c", "fd8",
    "85c", "60c", "MMU", "rt0", "rt1", "rt2", "rt3", "rt4", "rt5", "rt6", "rt7",
    "Ppt", "Ppb", "Ppr", "Pax", "Vpt", "Vpb", "Vpr", "Vax",
    "pgs", "pz0", "pz1", "xz0", "xz1", "td0", "td1", "nzo", "nzw",
    "fd0", "fd1", "rw0", "rw1" };
static const char rb_lbl[15][4] = { "824", "828", "82c", "838", "83c", "6d8", "85c",
    "840", "844", "84c", "850", "854", "858", "860", "020" };
static const char ck_lbl[MBX_CK_N][7] = { "reset", "A", "CTX", "PFX", "WAKE", "B", "post8", "S0",
    "S1", "S2", "S3/D1", "TRI", "preEOL", "EOL", "T", "I" };
static void head(int y) {
    fmt_str("R"); fmt_dec(var[variant].id); fmt_str(" "); fmt_str(var[variant].desc);
    if (page_total) { fmt_str(" p"); fmt_dec(page_no + 1); fmt_str("/"); fmt_dec(page_total); }
    fmt_line(4, y, WHITE);
}
static void busy(const char *what, uint32_t pct) {
    lcd_clear(0xff1a1a1a);
    page_total = 0;
    head(8);
    fmt_str("BUSY... "); fmt_str(what); fmt_str(" "); fmt_dec(pct); fmt_str("%");
    fmt_line(4, 24, WHITE);
    lcd_flush();
}
static uint32_t page_sum(uint32_t p) {
    const volatile uint32_t *w = (const volatile uint32_t *)(SCAN_BASE + (p << 12));
    uint32_t s = 0;
    for (uint32_t i = 0; i < 1024; ++i) s += w[i];
    return s;
}
/* written by the CPU between the two scans: kernel data/bss/stack (except
 * the MBX tables and pool), the LCD buffers, the target and trace ring */
static int cpu_page(uint32_t a) {
    if (mbx_gpu_page(a)) return 0;
    return (a + 4096u > (uint32_t)__data_start && a < (uint32_t)_stack_top) ||
           a - 0x08200000u < 0xe5400u || a - (uint32_t)target < W * H * 4u;
}
static void scan(int after) {
    for (uint32_t p = 0; p < SCAN_PAGES; ++p) {
        if (!(p & 1023)) busy(after ? "scan after" : "scan before", (p * 100u) >> 13);
        uint32_t s = page_sum(p), a = SCAN_BASE + (p << 12);
        if (!after) sums[p] = s;
        else if (s != sums[p]) {
            if (cpu_page(a)) ++n_cpu;
            else if (n_gpu++ < 16) chg[n_gpu - 1] = a;
        }
    }
}
/* frozen right after the frame, before any LCD or scan activity */
static void snap_target(void) {
    tgt_chg = tgt_mag = t_in = t_out = t_bdd = 0;
    tgt_idx = MBX_NONE;
    bx0 = W; by0 = H; bx1 = by1 = 0;
    for (uint32_t y = 0, i = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x, ++i) {
            uint32_t v = target[i];
            tgt_mag += v == 0xffff00ffu;
            if (v == 0xff00ff00u) continue;
            if (!tgt_chg++) { tgt_idx = i; tgt_val = v; }
            if (x < bx0) bx0 = x;
            if (x > bx1) bx1 = x;
            if (y < by0) by0 = y;
            by1 = y;
            if (x - TX < TILE_W && y - TY < TILE_H) { ++t_in; t_bdd += v == 0x00bdd6ffu; }
            else ++t_out;
        }
    for (uint32_t i = 0; i < 8; ++i) {
        raw[i] = target[TY * W + TX + i];
        raw[8 + i] = target[(TY + 1) * W + TX + i];
    }
}
/* computed from the frozen copies once the scans are done */
static void aux_stats(void) {
    const uint32_t *a = mbx_aux_copy(0), *s = sums;
    uint32_t f = mbx_dbg.first_page, span = mbx_dbg.last_page - f;
    a_nz = a_ff = a_raw = a_lo = a_hi = runs = 0;
    for (uint32_t i = 0; i < AUXW; ++i) {
        uint32_t v = a[i];
        sums[i] = v;
        a_nz += v != 0;
        a_ff += v == 0xffffffffu;
        a_raw += v - f <= span;
        a_lo += (v & 0xffffu) - f <= span;
        a_hi += (v >> 16) - f <= span;
    }
    a_hash = mbx_crc32(a, AUXW);
    static const uint16_t gaps[8] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (uint32_t g = 0; g < 8; ++g)
        for (uint32_t i = gaps[g], h = gaps[g]; i < AUXW; ++i) {
            uint32_t v = sums[i], j = i;
            for (; j >= h && sums[j - h] > v; j -= h) sums[j] = sums[j - h];
            sums[j] = v;
        }
    a_min = s[0];
    a_max = s[AUXW - 1];
    a_dist = 0;
    a_mode_n = 0;
    for (uint32_t i = 0; i < AUXW;) {
        uint32_t j = i;
        while (j < AUXW && s[j] == s[i]) ++j;
        ++a_dist;
        if (j - i > a_mode_n) { a_mode_n = j - i; a_mode = s[i]; }
        i = j;
    }
    for (uint32_t i = 0; i < AUXW;) {
        uint32_t e = i + 1, d = 0;
        if (e < AUXW) {
            d = a[e] - a[i];
            while (e < AUXW && a[e] - a[e - 1] == d) ++e;
        }
        if (runs < 20) { run_i[runs] = i; run_n[runs] = e - i; run_v[runs] = a[i]; run_d[runs] = d; }
        ++runs;
        i = e;
    }
}
static void run(void) {
    uint32_t f = var[variant].flags, *d = (uint32_t *)&mbx_dbg;
    for (uint32_t i = 0; i < sizeof mbx_dbg / 4; ++i) d[i] = 0;
    tgt_chg = tgt_mag = t_in = t_out = t_bdd = n_gpu = n_cpu = 0;
    tgt_idx = MBX_NONE;
    bx0 = W; by0 = H; bx1 = by1 = 0;
    for (uint32_t i = 0; i < 16; ++i) raw[i] = 0;
    mbx.scenes = mbx.jobs = mbx.timeouts = 0;
    busy("init", 0);
    mbx_variant = f | MBX_V_DIAG;
    ok = mbx_init();
    rc = -1;
    uint32_t n = 0;
    if (ok) {
        scan(0);
        for (int i = 0; i < W * H; ++i) target[i] = 0xff00ff00u;
        __asm__ volatile("dsb sy" ::: "memory");
        mbx_set_background(0xffff00ffu);
        mbx_set_target(target, W, H, W);
        s_idx[n++] = SEG_P;
        mbx_begin();
        for (uint32_t i = 0; i < 6; ++i)
            if (var[variant].segs & 1u << i) {
                if (n < MBX_SEGS) s_idx[n++] = i;
                mbx_submit_seg(seg_src[i], seg_len[i], seg_ck[i]);
            }
        rc = mbx_render(target, W, H, W) | mbx_wait_done(50000);
        __asm__ volatile("dsb sy" ::: "memory");
        snap_target();
        scan(1);
    }
    mbx_variant = 0;
    busy("stats", 0);
    if (mbx_dbg.ran & 1u) aux_stats();
    n_stage = 0;
    if (mbx_dbg.ran & 1u) raw_stage[n_stage++] = 0;
    for (uint32_t k = 1; k < 3; ++k) {
        x_n[k - 1] = 0;
        if (!(mbx_dbg.ran & 2u << k)) continue;
        const uint32_t *c = mbx_aux_copy(k), *p = mbx_aux_copy(k - 1);
        x_hash[k - 1] = mbx_crc32(c, AUXW);
        for (uint32_t i = 0; i < AUXW && x_n[k - 1] < 8; ++i)
            if (c[i] != p[i]) {
                uint32_t j = x_n[k - 1]++;
                x_i[k - 1][j] = i; x_o[k - 1][j] = p[i]; x_v[k - 1][j] = c[i];
            }
        if (mbx_dbg.aux_chg[k - 1]) raw_stage[n_stage++] = k;
    }
    const uint32_t *log = mbx_fifo_log();
    for (uint32_t s = 0; s < mbx_dbg.nseg && s < n; ++s) {
        uint32_t o = mbx_dbg.seg_log[s], w = mbx_dbg.seg_wr[s];
        if (o + w > MBX_LOG_WORDS) w = MBX_LOG_WORDS - o;
        s_crc[s] = mbx_crc32(seg_src[s_idx[s]], seg_len[s_idx[s]]);
        s_wcrc[s] = mbx_crc32(log + o, w);
    }
    pending = 0;
    t_done = USEC;
}
void mbx_test_enter(void) { pending = 1; }
void mbx_test_key(unsigned keys) {
    if (keys & BUTTON_RIGHT) { variant = variant + 1 == NVAR ? 0 : variant + 1; pending = 1; }
    if (keys & BUTTON_LEFT) { variant = variant ? variant - 1 : NVAR - 1; pending = 1; }
}
static void pair(uint32_t a, uint32_t b) { fmt_dec(a); fmt_str("/"); fmt_dec(b); }
static void at(uint32_t i) { if (i == MBX_NONE) fmt_str("NONE"); else fmt_dec(i); }
static void at_val(uint32_t i, uint32_t v) {
    at(i);
    if (i != MBX_NONE) { fmt_str("="); fmt_hex(v, 8); }
}
static void words(const uint32_t *w, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) { fmt_str(" "); fmt_hex(w[i], 8); }
}
static void dec4(uint32_t v) {
    for (uint32_t m = 1000; m > 1 && v < m; m = m == 1000 ? 100 : m == 100 ? 10 : 1) fmt_str("0");
    fmt_dec(v);
}
/* stage k (0 A, 1 B, 2 T, 3 I) snapshotted */
static int ran(uint32_t k) { return mbx_dbg.ran >> k & 1u; }
static void stage_dec(uint32_t k, uint32_t v) { if (ran(k)) fmt_dec(v); else fmt_str("NR"); }
static int line(int y) { fmt_line(4, y, WHITE); return y + 10; }
static void image(void) {
    /* portrait (px,py) -> landscape (431-py, px) */
    for (int py = 0; py < H; ++py)
        for (int px = 0; px < W; ++px) lcd_pixel(LW - 1 - py, px, target[py * W + px]);
}
static void aux_line(uint32_t k, int y) {
    fmt_str(k ? "auxI " : "auxT ");
    if (!ran(2 + k)) fmt_str("NOT_RUN");
    else {
        fmt_dec(mbx_dbg.aux_chg[k]); fmt_str(" @");
        at(mbx_dbg.aux_idx[k]);
        if (mbx_dbg.aux_idx[k] != MBX_NONE) {
            fmt_str(" "); fmt_hex(mbx_dbg.aux_old[k], 8); fmt_str(">"); fmt_hex(mbx_dbg.aux_new[k], 8);
        }
    }
    line(y);
}
/* first nonzero pool page x nonzero words at stage k (1 T, 2 I) */
static void pool_pg(uint32_t k) {
    if (!ran(k + 1)) fmt_str("NR");
    else if (!mbx_dbg.pnz[k]) fmt_str("0");
    else { fmt_str("pg"); fmt_dec(mbx_dbg.poff[k][0] >> 12); fmt_str("x"); fmt_dec(mbx_dbg.pnz[k]); }
}
/* the first summary line, 54 characters at most: pool at T (and I when
 * only I has data), w1 T/I, context slot result */
static const char *ctx_res(void) {
    return mbx_dbg.ctx_res == 1 ? "ok" : mbx_dbg.ctx_res == 2 ? "CTX TO" : "-";
}
static void summary(void) {
    image();
    for (int y = 0; y < 116; ++y)
        for (int x = 0; x < LW; ++x) lcd_pixel(x, y, 0xff000000u);
    fmt_str("R"); fmt_dec(var[variant].id); fmt_str(" "); fmt_str(var[variant].desc);
    fmt_str(" pool ");
    if (ran(3) && !mbx_dbg.pnz[1] && mbx_dbg.pnz[2]) { fmt_str("T0 I "); pool_pg(2); }
    else pool_pg(1);
    fmt_str(" w1 "); pair(mbx_dbg.rec_w1[0], mbx_dbg.rec_w1[1]);
    fmt_str(mbx_dbg.ctx_res == 2 ? " " : " ctx "); fmt_str(ctx_res());
    fmt_line(4, 2, WHITE);
    fmt_str(!ok ? "NO GPU" : rc ? "ERR" : "OK"); fmt_str(" ta "); fmt_hex(mbx.ta_status, 8);
    fmt_str(" job "); fmt_hex(mbx.job_status, 8);
    fmt_str(" fifo "); fmt_dec((mbx.ta_status >> 16) & 0xffu);
    fmt_str(" to "); fmt_dec(mbx.timeouts);
    line(12);
    uint32_t e = 1;
    while (e < 8 && !(mbx_dbg.st[e] & 0x40u)) ++e;
    fmt_str("job "); fmt_dec(mbx.job_us); fmt_str(" us ev40 @");
    fmt_str(e < 8 ? ev_site[e] : "none"); fmt_str(" st "); fmt_hex(mbx.status, 8);
    line(22);
    fmt_str("pool A/T/I ");
    for (uint32_t k = 0; k < 3; ++k) { if (k) fmt_str("/"); stage_dec(k ? k + 1 : 0, mbx_dbg.pnz[k]); }
    fmt_str(" auxnz "); pair(mbx_dbg.aux_nz[0], mbx_dbg.aux_nz[1]);
    fmt_str(" fl "); pair(mbx_dbg.flags_diff[0], mbx_dbg.flags_diff[1]);
    line(32);
    fmt_str("w1 "); pair(mbx_dbg.rec_w1[0], mbx_dbg.rec_w1[1]);
    fmt_str(" T@"); at_val(mbx_dbg.w1_idx[0], mbx_dbg.w1_val[0]);
    fmt_str(" I@"); at_val(mbx_dbg.w1_idx[1], mbx_dbg.w1_val[1]);
    line(42);
    aux_line(0, 52);
    aux_line(1, 62);
    fmt_str("tgtT "); fmt_dec(mbx_dbg.tgt_chg); fmt_str(" @");
    at_val(mbx_dbg.tgt_idx, mbx_dbg.tgt_val);
    line(72);
    fmt_str("tgtI "); fmt_dec(tgt_chg); fmt_str(" mag "); fmt_dec(tgt_mag); fmt_str(" @");
    at_val(tgt_idx, tgt_val);
    line(82);
    fmt_str("bbox ");
    if (!tgt_chg) fmt_str("NONE");
    else {
        fmt_dec(bx0); fmt_str(","); fmt_dec(by0); fmt_str("-"); fmt_dec(bx1); fmt_str(","); fmt_dec(by1);
    }
    fmt_str(" in "); fmt_dec(t_in); fmt_str(" out "); fmt_dec(t_out);
    fmt_str(" bdd "); fmt_dec(t_bdd);
    line(92);
    fmt_str("tbl T/I "); stage_dec(2, mbx_dbg.tbl_nz[0]); fmt_str("/"); stage_dec(3, mbx_dbg.tbl_nz[1]);
    fmt_str(" gap "); stage_dec(2, mbx_dbg.tbl_gap[0]); fmt_str("/"); stage_dec(3, mbx_dbg.tbl_gap[1]);
    fmt_str(" fifomax "); fmt_dec(mbx_dbg.fifo_max); fmt_str(" bells "); fmt_dec(mbx_dbg.bells);
    line(102);
}
static void plain(void) {
    image();
    head(2);
}
/* registers, readbacks at B/T/I, scan pages */
static void dump(void) {
    const uint32_t *d = (const uint32_t *)&mbx_dbg;
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    for (uint32_t i = 0; i < 52; ++i) {
        fmt_str(lbl[i]); fmt_str(" "); fmt_hex(d[i], 8);
        if ((i & 3) == 3) y = line(y); else fmt_str(" ");
    }
    if (!ran(1)) { fmt_str("B NOT_RUN"); y = line(y); }
    else
        for (uint32_t i = 0; i < 15; ++i) {
            fmt_str(rb_lbl[i]); fmt_str(" "); fmt_hex(mbx_dbg.rb_b[i], 8);
            if ((i & 3) == 3 || i == 14) y = line(y); else fmt_str(" ");
        }
    fmt_str("T840 ");
    if (ran(2)) { fmt_hex(mbx_dbg.rb_t[0], 8); fmt_str(" 844 "); fmt_hex(mbx_dbg.rb_t[1], 8); }
    else fmt_str("NOT_RUN");
    fmt_str(" I608 ");
    if (ran(3)) { fmt_hex(mbx_dbg.rb_i[0], 8); fmt_str(" 60c "); fmt_hex(mbx_dbg.rb_i[1], 8); }
    else fmt_str("NOT_RUN");
    y = line(y);
    fmt_str("tgt "); fmt_hex((uint32_t)target, 8); fmt_str(">"); fmt_hex(MBX_VA(target), 8);
    fmt_str(" pages gpu "); fmt_dec(n_gpu); fmt_str(" cpu "); fmt_dec(n_cpu);
    y = line(y);
    for (uint32_t i = 0; i < 16 && i < n_gpu; ++i) {
        fmt_hex(chg[i] >> 12, 5); fmt_str(" ");
        if (i == 7 || i + 1 == n_gpu || i == 15) y = line(y);
    }
}
/* aux at A: addresses, statistics, sample and page-range windows */
static void aux_a(void) {
    uint32_t mark[AUXW / 4 / 32];
    for (uint32_t i = 0; i < AUXW / 4 / 32; ++i) mark[i] = 0;   /* no memset */
    uint32_t f = mbx_dbg.first_page, l = mbx_dbg.last_page, n = 0;
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    fmt_str("A PA "); fmt_hex(mbx_dbg.pa[3], 8); fmt_str(" VA "); fmt_hex(mbx_dbg.va[3], 8);
    fmt_str(" 828 "); fmt_hex(f, 8); fmt_str(" 82c "); fmt_hex(l, 8);
    y = line(y);
    if (!ran(0)) { fmt_str("AUX A NOT_RUN"); line(y); return; }
    fmt_str("nz "); fmt_dec(a_nz); fmt_str(" distinct "); fmt_dec(a_dist);
    fmt_str(" hash "); fmt_hex(a_hash, 8);
    y = line(y);
    fmt_str("min "); fmt_hex(a_min, 8); fmt_str(" max "); fmt_hex(a_max, 8);
    fmt_str(" mode "); fmt_hex(a_mode, 8); fmt_str(" x"); fmt_dec(a_mode_n);
    y = line(y);
    for (uint32_t i = 0; i < 10; ++i) mark[0] |= 1u << i;          /* 0..36 */
    mark[AUXW / 4 / 32 - 1] |= 0xf0000000u;                       /* 4080..4092 */
    uint32_t lo = f >= 2 ? f - 2 : 0, hi = l + 2 < AUXW ? l + 2 : AUXW - 1;
    if (f < AUXW)
        for (uint32_t w = lo >> 2; w <= hi >> 2; ++w) mark[w >> 5] |= 1u << (w & 31);
    for (uint32_t w = 0; w < AUXW / 4; ++w) n += mark[w >> 5] >> (w & 31) & 1u;
    fmt_str("ffffffff x"); fmt_dec(a_ff); fmt_str(" in[f,l] raw "); fmt_dec(a_raw);
    fmt_str(" lo "); fmt_dec(a_lo); fmt_str(" hi "); fmt_dec(a_hi); fmt_str(" w"); fmt_dec(n);
    y = line(y);
    const uint32_t *a = mbx_aux_copy(0);
    for (uint32_t w = 0; w < AUXW / 4 && y <= 216; ++w) {
        if (!(mark[w >> 5] >> (w & 31) & 1u)) continue;
        fmt_str("i="); dec4(4 * w); words(a + 4 * w, 4);
        y = line(y);
    }
}
/* aux T and I against the previous stage */
static void aux_ti(void) {
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    for (uint32_t k = 0; k < 2; ++k) {
        fmt_str(k ? "I" : "T");
        if (!ran(2 + k)) { fmt_str(" NOT_RUN"); y = line(y) + 10; continue; }
        fmt_str(" chg "); fmt_dec(mbx_dbg.aux_chg[k]); fmt_str(k ? " vs T" : " vs A");
        fmt_str(" hash "); fmt_hex(x_hash[k], 8);
        y = line(y);
        for (uint32_t j = 0; j < x_n[k]; ++j) {
            dec4(x_i[k][j]); fmt_str(" "); fmt_hex(x_o[k][j], 8); fmt_str(">"); fmt_hex(x_v[k][j], 8);
            if (j & 1 || j + 1 == x_n[k]) y = line(y); else fmt_str(" ");
        }
        y += 10;
    }
    fmt_str("RAW pages: A, T/I only when changed");
    line(y);
}
/* constant-delta runs of aux A (index, length, first value, delta) */
static void aux_runs(void) {
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    if (!ran(0)) { fmt_str("AUX A NOT_RUN"); line(y); return; }
    fmt_str("AUX A runs "); fmt_dec(runs); fmt_str(" (i n v delta)");
    y = line(y);
    for (uint32_t r = 0; r < runs && r < 20; ++r) {
        fmt_str("i="); dec4(run_i[r]); fmt_str(" n="); fmt_dec(run_n[r]);
        fmt_str(" v="); fmt_hex(run_v[r], 8); fmt_str(" d="); fmt_hex(run_d[r], 8);
        y = line(y);
    }
}
/* the 840 table at T/I, region counts and records, raw tile rows */
static void table(void) {
    static const uint16_t rec_no[7] = { 0, 1, 29, 30, 31, 808, 809 };
    static const uint32_t raw_no[4] = { TY * W + TX, TY * W + TX + 4,
                                        (TY + 1) * W + TX, (TY + 1) * W + TX + 4 };
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    for (uint32_t k = 0; k < 2; ++k) {
        fmt_str(k ? "I" : "T"); fmt_str(" tbl");
        if (!ran(2 + k)) { fmt_str(" NOT_RUN"); y = line(y); continue; }
        fmt_str(" nz "); fmt_dec(mbx_dbg.tbl_nz[k]); fmt_str(" gap "); fmt_dec(mbx_dbg.tbl_gap[k]);
        fmt_str(" hash "); fmt_hex(mbx_dbg.tbl_hash[k], 8);
        y = line(y);
        uint32_t m = mbx_dbg.tbl_nz[k] < 8 ? mbx_dbg.tbl_nz[k] : 8;
        for (uint32_t j = 0; j < m; ++j) {
            fmt_hex(mbx_dbg.tbl_off[k][j], 3); fmt_str(" "); fmt_hex(mbx_dbg.tbl_val[k][j], 8);
            if ((j & 3) == 3 || j + 1 == m) y = line(y); else fmt_str(" ");
        }
    }
    for (uint32_t k = 0; k < 2; ++k) {
        fmt_str(k ? "I" : "T");
        if (!ran(2 + k)) { fmt_str(" NOT_RUN"); y = line(y); continue; }
        fmt_str(" b29 "); fmt_dec(mbx_dbg.b29[k]);
        fmt_str(" b31 "); fmt_dec(mbx_dbg.b31[k]); fmt_str("@"); at(mbx_dbg.b31_idx[k]);
        fmt_str(" crd "); fmt_dec(mbx_dbg.crd_diff[k]);
        fmt_str(" fl "); fmt_dec(mbx_dbg.flags_diff[k]);
        fmt_str(" w1 "); fmt_dec(mbx_dbg.rec_w1[k]);
        y = line(y);
    }
    for (uint32_t n = 0; n < 14; ++n) {
        uint32_t k = n / 7, i = n - 7 * k;
        fmt_str(k ? "I" : "T"); fmt_dec(rec_no[i]); fmt_str(" ");
        fmt_hex(mbx_dbg.rec[k][i][0], 8); fmt_str(" "); fmt_hex(mbx_dbg.rec[k][i][1], 8);
        if (n & 1) y = line(y); else fmt_str("  ");
    }
    for (uint32_t i = 0; i < 4; ++i) {
        fmt_str("r"); fmt_dec(raw_no[i]); words(raw + 4 * i, 4);
        y = line(y);
    }
}
/* parameter pool at A, T and I */
static void pool(void) {
    static const char st[3][2] = { "A", "T", "I" };
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    fmt_str("pool pages "); fmt_dec(mbx_dbg.pages); fmt_str(" 828 "); fmt_hex(mbx_dbg.first_page, 5);
    fmt_str(" 82c "); fmt_hex(mbx_dbg.last_page, 5); fmt_str(" VA "); fmt_hex(mbx_dbg.va[2], 8);
    y = line(y);
    for (uint32_t k = 0; k < 3; ++k) {
        fmt_str(st[k]);
        if (!ran(k ? k + 1 : 0)) { fmt_str(" NOT_RUN"); y = line(y); continue; }
        fmt_str(" nz "); fmt_dec(mbx_dbg.pnz[k]); fmt_str(" tpl "); fmt_dec(mbx_dbg.ptpl[k]);
        fmt_str(" mask "); fmt_hex(mbx_dbg.pmask[k], 8);
        y = line(y);
        uint32_t m = mbx_dbg.pnz[k] < 8 ? mbx_dbg.pnz[k] : 8;
        for (uint32_t j = 0; j < m; ++j) {
            fmt_hex(mbx_dbg.poff[k][j], 5); fmt_str(" "); fmt_hex(mbx_dbg.pval[k][j], 8);
            if (j % 3 == 2 || j + 1 == m) y = line(y); else fmt_str(" ");
        }
    }
}
/* segments as submitted, EOL, status checkpoints */
static void submission(void) {
    uint32_t data = 0, first15 = MBX_CK_N;
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    for (uint32_t s = 0; s < mbx_dbg.nseg; ++s) data += mbx_dbg.seg_wr[s];
    fmt_str("R"); fmt_dec(var[variant].id); fmt_str(" seg "); fmt_dec(mbx_dbg.nseg);
    fmt_str(" data "); fmt_dec(data); fmt_str(" all "); fmt_dec(data + mbx_dbg.eol_wr);
    fmt_str(" bells "); fmt_dec(mbx_dbg.bells); fmt_str(" eolbell "); fmt_dec(mbx_dbg.eol_bell);
    y = line(y);
    for (uint32_t s = 0; s < mbx_dbg.nseg && s < MBX_SEGS; ++s) {
        uint32_t i = s_idx[s], wr = mbx_dbg.seg_wr[s];
        fmt_str(seg_name[i]); fmt_str(" "); fmt_dec(wr); fmt_str("/"); fmt_dec(mbx_dbg.seg_words[s]);
        fmt_str(" "); fmt_hex(s_crc[s], 8); fmt_str(" "); fmt_hex(s_wcrc[s], 8);
        fmt_str(s_crc[s] != s_wcrc[s] || wr != seg_len[i] ? " BAD " :
                !seg_crc[i] ? " -- " : s_crc[s] == seg_crc[i] ? " ok " : " BAD ");
        fmt_hex(seg_src[i][0], 8); fmt_str(" "); fmt_hex(seg_src[i][seg_len[i] - 1], 8);
        y = line(y);
    }
    const uint32_t *log = mbx_fifo_log();
    fmt_str("EOL wr "); fmt_dec(mbx_dbg.eol_wr); fmt_str(" bell "); fmt_dec(mbx_dbg.eol_bell);
    fmt_str(" log "); fmt_dec(mbx_dbg.log_n); fmt_str(" last ");
    fmt_hex(mbx_dbg.log_n ? log[mbx_dbg.log_n - 1] : 0, 8);
    fmt_str(" fmax "); fmt_dec(mbx_dbg.fifo_max);
    y = line(y);
    fmt_str("ST name 12c fifo us(since init)");
    y = line(y);
    for (uint32_t c = 0; c < MBX_CK_N; ++c) {
        fmt_str(ck_lbl[c]); fmt_str(" ");
        if (!(mbx_dbg.ck_mask >> c & 1u)) fmt_str("-");
        else {
            uint32_t v = mbx_dbg.ck_st[c];
            if (v & 0x8000u && first15 == MBX_CK_N) first15 = c;
            fmt_hex(v, 8); fmt_str(" "); fmt_dec((v >> 16) & 0xffu); fmt_str(" "); fmt_dec(mbx_dbg.ck_us[c]);
        }
        if (c & 1 || c + 1 == MBX_CK_N) y = line(y); else fmt_str("  ");
    }
    fmt_str("bit15 first @ "); fmt_str(first15 < MBX_CK_N ? ck_lbl[first15] : "none");
    line(y);
}
/* cold context-slot operation, readback-after-write, ISP regs at B */
static void ctx_page(void) {
    static const char rl[5][4] = { "12c", "810", "81c", "820", "864" };
    static const char el[10][4] = { "608", "60c", "610", "63c", "650", "654", "658",
                                    "65c", "660", "6dc" };
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    fmt_str("CTX slot0 ");
    if (!mbx_dbg.ctx_res) { fmt_str("NOT_RUN"); y = line(y); }
    else {
        fmt_str(ctx_res()); fmt_str(" st "); fmt_hex(mbx_dbg.ctx_st, 8);
        fmt_str(" "); fmt_dec(mbx_dbg.ctx_us); fmt_str(" us");
        y = line(y);
        for (uint32_t k = 0; k < 2; ++k) {
            const uint32_t *v = k ? mbx_dbg.ctx_post : mbx_dbg.ctx_pre;
            fmt_str(k ? "post" : "pre ");
            for (uint32_t i = 0; i < 5; ++i) {
                fmt_str(" "); fmt_str(rl[i]); fmt_str(" "); fmt_hex(v[i], 8);
                if (i == 2) { y = line(y); fmt_str("    "); }
            }
            y = line(y);
        }
    }
    if (!ran(1)) { fmt_str("B NOT_RUN"); line(y); return; }
    fmt_str("854 w 3727c5ac imm "); fmt_hex(mbx_dbg.rb_w[0], 8); fmt_str(" B "); fmt_hex(mbx_dbg.rb_b[11], 8);
    y = line(y);
    fmt_str("860 w ");
    fmt_str("ISP only");
    fmt_str(" imm "); fmt_hex(mbx_dbg.rb_w[1], 8);
    fmt_str(" B "); fmt_hex(mbx_dbg.rb_b[13], 8);
    y = line(y);
    fmt_str("ISP regs at B");
    y = line(y);
    for (uint32_t i = 0; i < 10; ++i) {
        fmt_str(el[i]); fmt_str(" "); fmt_hex(mbx_dbg.rb_e[i], 8);
        if ((i & 3) == 3 || i == 9) y = line(y); else fmt_str(" ");
    }
}
/* raw aux copy, RAW_PAGE words per page */
static void aux_raw(uint32_t r) {
    static const char st[3][2] = { "A", "T", "I" };
    uint32_t k = 0;
    while (r >= RAW_PER) { r -= RAW_PER; ++k; }
    const uint32_t *a = mbx_aux_copy(raw_stage[k]);
    uint32_t i0 = r * RAW_PAGE;
    int y = 12;
    lcd_clear(0xff1a1a1a);
    head(2);
    fmt_str("AUX RAW "); fmt_str(st[raw_stage[k]]); fmt_str(" "); fmt_dec(r + 1); fmt_str("/");
    fmt_dec(RAW_PER); fmt_str(" i="); fmt_dec(i0); fmt_str(".."); fmt_dec(i0 + RAW_PAGE - 1 < AUXW ? i0 + RAW_PAGE - 1 : AUXW - 1);
    y = line(y);
    for (uint32_t i = i0; i < i0 + RAW_PAGE && i < AUXW; i += 4) {
        dec4(i); words(a + i, 4);
        y = line(y);
    }
}
/* RAM report for NanoOSLoader (report.h): every text page, then the full
 * aux copy of each stage that ran; a re-run overwrites the case's slot */
static uint32_t rpt_len;
static void rpt_busy(uint32_t pct) {
    char *p = fmt_out;
    fmt_out = 0;
    busy("report", pct);
    fmt_out = p;
}
static void rpt_line(void) { fmt_line(0, 0, 0); }
static void rpt_save(void) {
    static void (*const pg[9])(void) = { summary, dump, aux_a, aux_ti, aux_runs, table, pool,
                                         submission, ctx_page };
    static const char pg_name[9][11] = { "SUMMARY", "REGS", "AUX A", "AUX T/I", "AUX RUNS",
                                         "TABLE", "POOL", "SUBMISSION", "CTX/RB" };
    static const char st[3][2] = { "A", "T", "I" };
    volatile struct rpt_hdr *h = RPT_HDR;
    if (!rpt_valid()) {
        for (uint32_t i = 0; i < sizeof *h / 4; ++i) ((volatile uint32_t *)h)[i] = 0;
        h->slot_size = RPT_SLOT_SIZE;
    }
    char *p0 = RPT_SLOT(variant);
    fmt_out = p0;
    fmt_end = p0 + RPT_SLOT_SIZE;
    for (uint32_t k = 0; k < 9; ++k) {
        rpt_busy(k * 8u);
        fmt_str("== "); fmt_str(pg_name[k]); fmt_str(" ==");
        rpt_line();
        pg[k]();
    }
    for (uint32_t k = 0; k < 3; ++k) {
        rpt_busy(72u + k * 9u);
        fmt_str("== AUX RAW "); fmt_str(st[k]);
        if (!ran(k ? k + 1 : 0)) { fmt_str(" NOT_RUN =="); rpt_line(); continue; }
        fmt_str(" (index, 4 words) ==");
        rpt_line();
        const uint32_t *a = mbx_aux_copy(k);
        for (uint32_t i = 0; i < AUXW; i += 4) { dec4(i); words(a + i, 4); rpt_line(); }
    }
    fmt_str("== END R"); fmt_dec(var[variant].id); fmt_str(" ==");
    rpt_line();
    rpt_len = (uint32_t)(fmt_out - p0);
    fmt_out = 0;
    rpt_busy(99);
    h->id[variant] = var[variant].id;
    h->len[variant] = rpt_len;
    h->sum[variant] = rpt_sum(p0, rpt_len);
    h->magic[0] = RPT_MAGIC0;
    h->magic[1] = RPT_MAGIC1;
    h->hdr_sum = rpt_hdr_sum();
}
void mbx_test_draw(void) {
    if (pending) { run(); rpt_save(); }
    /* ten pages of ~4.2 s, then the raw aux pages of ~2.1 s each */
    uint32_t cycle = (NPAGES << 22) + ((n_stage * RAW_PER) << 21);
    while (USEC - t_done >= cycle) t_done += cycle;
    uint32_t e = USEC - t_done;
    page_total = NPAGES + n_stage * RAW_PER;
    page_no = e < NPAGES << 22 ? e >> 22 : NPAGES + ((e - (NPAGES << 22)) >> 21);
    switch (page_no) {
    case 0: summary(); break;
    case 1: dump(); break;
    case 2: aux_a(); break;
    case 3: aux_ti(); break;
    case 4: aux_runs(); break;
    case 5: table(); break;
    case 6: pool(); break;
    case 7: submission(); break;
    case 8: ctx_page(); break;
    case 9: plain(); break;
    default: aux_raw(page_no - NPAGES); break;
    }
    fmt_str(rpt_len < RPT_SLOT_SIZE ? "RPT SAVED R" : "RPT TRUNC R"); fmt_dec(var[variant].id);
    fmt_str(" "); fmt_dec(rpt_len); fmt_str("B  Vol+/-: case  Home: back");
    fmt_line(4, 226, WHITE);
    lcd_present();
}
