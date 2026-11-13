/* Instruction timing loops for the EspRacer's ESP32-S3 (LX7): each bench_NAME(iterations, pointer) runs its body
   16 times per pass of a zero-overhead loop, from internal RAM. bench.c reads the cycle counter around a call and
   divides by iterations x 16: cycles per body. A body of one instruction that feeds itself measures latency; a
   body of independent instructions measures throughput. */
    .section .iram1.bench, "ax"
    .literal_position

.macro BENCH name, setup, body
    .global bench_\name
    .type bench_\name, @function
    .align 4
bench_\name:
    entry a1, 48
    \setup
    loop a2, .Lend_\name
    .rept 16
    \body
    .endr
.Lend_\name:
    retw
    .size bench_\name, . - bench_\name
.endm

/* integer unit */
BENCH nop,        "movi a4, 1",                              "nop"
BENCH add_lat,    "movi a3, 1; movi a4, 1",                  "add a3, a3, a4"
BENCH add_thr,    "movi a4, 1",                              "add a5, a5, a4; add a6, a6, a4; add a7, a7, a4; add a8, a8, a4"
BENCH addx2_lat,  "movi a3, 1; movi a4, 1",                  "addx2 a3, a3, a4"
BENCH mull_lat,   "movi a3, 3; movi a4, 5",                  "mull a3, a3, a4"
BENCH mull_thr,   "movi a4, 5",                              "mull a5, a5, a4; mull a6, a6, a4; mull a7, a7, a4; mull a8, a8, a4"
BENCH mulsh_lat,  "movi a3, 3; movi a4, 5",                  "mulsh a3, a3, a4"
BENCH mul16_lat,  "movi a3, 3; movi a4, 5",                  "mul16u a3, a3, a4"
BENCH quou_thr,   "movi a3, 0x7fffffff; movi a4, 7",         "quou a5, a3, a4"
BENCH quou_lat,   "movi a3, 0x7fffffff; movi a4, 7; movi a6, 0x40000000", "quou a3, a3, a4; or a3, a3, a6"
BENCH quos_thr,   "movi a3, 0x7fffffff; movi a4, 7",         "quos a5, a3, a4"
BENCH remu_thr,   "movi a3, 0x7fffffff; movi a4, 7",         "remu a5, a3, a4"
BENCH quou_small, "movi a3, 300; movi a4, 7",                "quou a5, a3, a4"
BENCH ssl_sll,    "movi a3, 1; movi a4, 1",                  "ssl a4; sll a3, a3"
BENCH ssr_srl,    "movi a3, -1; movi a4, 1",                 "ssr a4; srl a5, a3"
BENCH extui_lat,  "movi a3, 12345",                          "extui a3, a3, 1, 16"
BENCH srai_lat,   "movi a3, 12345",                          "srai a3, a3, 1"
BENCH nsau_lat,   "movi a3, 12345",                          "nsau a3, a3"
BENCH clamps_lat, "movi a3, 12345",                          "clamps a3, a3, 7"
BENCH minmax,     "movi a3, 12345; movi a4, 100; movi a5, 50", "min a3, a3, a4; max a3, a3, a5"
BENCH moveqz,     "movi a3, 1; movi a4, 0; movi a5, 2",      "moveqz a3, a5, a4"
/* branches: a5 = 0 */
/* each body ends with an add: a branch onto the loop's end would leave the zero-overhead loop */
BENCH br_taken,   "movi a5, 0; movi a4, 1",                  "beqz a5, 1f; nop; 1: add a6, a6, a4"
BENCH br_not,     "movi a5, 0; movi a4, 1",                  "bnez a5, 1f; nop; 1: add a6, a6, a4"
BENCH bbci_taken, "movi a5, 0; movi a4, 1",                  "bbci a5, 0, 1f; nop; 1: add a6, a6, a4"

/* memory: a3 points at a word holding its own address (load latency), or at a buffer (streams) */
BENCH l32i_lat,   "",                                        "l32i a3, a3, 0"
BENCH l32i_thr,   "",                                        "l32i a5, a3, 0; l32i a6, a3, 4; l32i a7, a3, 8; l32i a8, a3, 12"
BENCH l8_use,     "movi a6, 0",                              "l8ui a5, a3, 0; add a6, a6, a5"
BENCH l16_use,    "movi a6, 0",                              "l16ui a5, a3, 0; add a6, a6, a5"
BENCH l16_addr,   "movi a6, 0",                              "l16ui a5, a3, 4; addx2 a7, a5, a3; l16ui a6, a7, 0"
BENCH s16_stream, "movi a4, 7",                              "s16i a4, a3, 0; addi a3, a3, 2"
BENCH s32_stream, "movi a4, 7",                              "s32i a4, a3, 0; addi a3, a3, 4"
BENCH ld_st16,    "",                                        "l16ui a4, a3, 0; s16i a4, a3, 0; addi a3, a3, 2"

/* floating point unit (f1 = 1.0) */
#define FSETUP "movi a4, 0x3f800000; wfr f0, a4; wfr f1, a4; wfr f2, a4; wfr f3, a4; wfr f4, a4; wfr f5, a4"
BENCH adds_lat,   FSETUP,                                    "add.s f0, f0, f1"
BENCH adds_thr,   FSETUP,                                    "add.s f2, f2, f1; add.s f3, f3, f1; add.s f4, f4, f1; add.s f5, f5, f1"
BENCH muls_lat,   FSETUP,                                    "mul.s f0, f0, f1"
BENCH muls_thr,   FSETUP,                                    "mul.s f2, f2, f1; mul.s f3, f3, f1; mul.s f4, f4, f1; mul.s f5, f5, f1"
BENCH madds_lat,  FSETUP,                                    "madd.s f0, f1, f1"
BENCH madds_thr,  FSETUP,                                    "madd.s f2, f1, f1; madd.s f3, f1, f1; madd.s f4, f1, f1; madd.s f5, f1, f1"
BENCH recip0_lat, FSETUP,                                    "recip0.s f0, f0"
BENCH rsqrt0_lat, FSETUP,                                    "rsqrt0.s f0, f0"
BENCH div0_lat,   FSETUP,                                    "div0.s f0, f0"
BENCH divn_lat,   FSETUP,                                    "divn.s f0, f1, f1"
BENCH f2i_i2f,    "movi a3, 12345",                          "float.s f0, a3, 0; trunc.s a3, f0, 0"
BENCH trunc_thr,  FSETUP,                                    "trunc.s a5, f1, 15; trunc.s a6, f1, 15; trunc.s a7, f1, 15; trunc.s a8, f1, 15"
BENCH float_thr,  "movi a3, 12345",                          "float.s f2, a3, 0; float.s f3, a3, 0; float.s f4, a3, 0; float.s f5, a3, 0"
BENCH wfr_rfr,    "movi a3, 12345",                          "wfr f0, a3; rfr a3, f0"
BENCH olt_br,     FSETUP,                                    "olt.s b0, f0, f1; bt b0, 1f; nop; 1: add a6, a6, a4"
BENCH lsi_use,    FSETUP,                                    "lsi f2, a3, 0; add.s f3, f3, f2"
BENCH adds_rfr,   FSETUP,                                    "add.s f0, f0, f1; rfr a5, f0"
BENCH muls_trunc, FSETUP,                                    "mul.s f0, f0, f1; trunc.s a5, f0, 0"
