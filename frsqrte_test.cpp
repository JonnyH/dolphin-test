#include <gccore.h>
#include <ogcsys.h>
#include <stdlib.h>

#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <debug.h>
#include <iostream>
#include <random>

#include <ppu_intrinsics.h>

static void *xfb = NULL;

u32 first_frame = 1;
GXRModeObj *rmode;
vu16 oldstate;
vu16 keystate;
vu16 keydown;
vu16 keyup;
PADStatus pad[4];

/* ApproximateReciprocalSquareRoot() is taken from the dolphin emulator, which is GPL2 licensed */

struct BaseAndDec {
  int m_base;
  int m_dec;
};

const std::array<BaseAndDec, 32> frsqrte_expected = {{

    {0x3ffa000, 0x7a4}, {0x3c29000, 0x700}, {0x38aa000, 0x670},
    {0x3572000, 0x5f2}, {0x3279000, 0x584}, {0x2fb7000, 0x524},
    {0x2d26000, 0x4cc}, {0x2ac0000, 0x47e}, {0x2881000, 0x43a},
    {0x2665000, 0x3fa}, {0x2468000, 0x3c2}, {0x2287000, 0x38e},
    {0x20c1000, 0x35e}, {0x1f12000, 0x332}, {0x1d79000, 0x30a},
    {0x1bf4000, 0x2e6}, {0x1a7e800, 0x568}, {0x17cb800, 0x4f3},
    {0x1552800, 0x48d}, {0x130c000, 0x435}, {0x10f2000, 0x3e7},
    {0x0eff000, 0x3a2}, {0x0d2e000, 0x365}, {0x0b7c000, 0x32e},
    {0x09e5000, 0x2fc}, {0x0867000, 0x2d0}, {0x06ff000, 0x2a8},
    {0x05ab800, 0x283}, {0x046a000, 0x261}, {0x0339800, 0x243},
    {0x0218800, 0x226}, {0x0105800, 0x20b},
}};

double ApproximateReciprocalSquareRoot(double val) {
  union {
    double valf;
    s64 vali;
  };
  valf = val;
  s64 mantissa = vali & ((1LL << 52) - 1);
  s64 sign = vali & (1ULL << 63);
  s64 exponent = vali & (0x7FFLL << 52);

  // Special case 0
  if (mantissa == 0 && exponent == 0)
    return sign ? -std::numeric_limits<double>::infinity()
                : std::numeric_limits<double>::infinity();
  // Special case NaN-ish numbers
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0) {
      if (sign)
        return std::numeric_limits<double>::quiet_NaN();

      return 0.0;
    }

    return 0.0 + valf;
  }

  // Negative numbers return NaN
  if (sign)
    return std::numeric_limits<double>::quiet_NaN();

  if (!exponent) {
    // "Normalize" denormal values
    do {
      exponent -= 1LL << 52;
      mantissa <<= 1;
    } while (!(mantissa & (1LL << 52)));
    mantissa &= (1LL << 52) - 1;
    exponent += 1LL << 52;
  }

  bool odd_exponent = !(exponent & (1LL << 52));
  exponent =
      ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);

  int i = (int)(mantissa >> 37);
  vali = sign | exponent;
  int index = i / 2048 + (odd_exponent ? 16 : 0);
  const auto &entry = frsqrte_expected[index];
  vali |= (s64)(entry.m_base - entry.m_dec * (i % 2048)) << 26;
  return valf;
}

#define COUNT 10000000
#define PRINT_STEP 10000

#define SEED 133780085

int main() {
  unsigned fail_count = 0;
  unsigned skipped = 0;

  VIDEO_Init();

  rmode = VIDEO_GetPreferredMode(NULL);

  PAD_Init();

  xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));

  VIDEO_Configure(rmode);

  VIDEO_SetNextFramebuffer(xfb);

  VIDEO_SetBlack(FALSE);
  VIDEO_Flush();
  VIDEO_WaitVSync();
  if (rmode->viTVMode & VI_NON_INTERLACE)
    VIDEO_WaitVSync();

  console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight,
               rmode->fbWidth * 2);

  printf("Testing frsqrtex\n");

  std::default_random_engine rng(SEED);
  std::uniform_int_distribution<uint32_t> dist(
      0, std::numeric_limits<uint32_t>::max());

  for (unsigned i = 0; i < COUNT; i++) {
    // We don't want to get a numerical distribution, but instead a sampling of
    // all bit patterns - so generate 2 ints and convert that to a double
    // instead
    uint32_t rand_ints[2];
    double rand_double;
    rand_ints[0] = dist(rng);
    rand_ints[1] = dist(rng);

    static_assert(sizeof(rand_double) == sizeof(rand_ints),
                  "Size mismatch between uint32_t[2] and double");

    memcpy(&rand_double, &rand_ints[0], sizeof(rand_double));

    if (std::isnan(rand_double)) {
      // Skip patterns that happen to be not-a-number
      skipped++;
      continue;
    }

    double inst_result = __frsqrte(rand_double);
    double sw_result = ApproximateReciprocalSquareRoot(rand_double);

    if (inst_result != sw_result) {
      if (std::isnan(inst_result) && std::isnan(sw_result)) {
        // Skip these, as NAN == NAN is always false
        continue;
      }
      fail_count++;
      // To force the bits to be printed in hex
      uint64_t integer_result;
      static_assert(sizeof(integer_result) == sizeof(double),
                    "Size mismatch between uint64_t and double");
      memcpy(&integer_result, &rand_double, sizeof(rand_double));
      printf("%u: Mismatch for %f (0x%" PRIx64 ") \n", i, rand_double,
             integer_result);
      memcpy(&integer_result, &inst_result, sizeof(inst_result));
      printf("\tinstruction returned %f (0x%" PRIx64 ")\n", inst_result,
             integer_result);
      memcpy(&integer_result, &sw_result, sizeof(sw_result));
      printf("\tSW implementation returned %f (0x%" PRIx64 ")\n", sw_result,
             integer_result);
    }

    if ((i % PRINT_STEP) == 0) {
      printf("Test loop %u\n", i);

      VIDEO_WaitVSync();
      PAD_ScanPads();

      int buttonsDown = PAD_ButtonsDown(0);

      if (buttonsDown & PAD_BUTTON_START) {
        exit(0);
      }
    }
  }
  printf("Test loop finisned - %u failures (%u skipped due to NAN)\n",
         fail_count, skipped);
  while (true) {

    VIDEO_WaitVSync();
    PAD_ScanPads();

    int buttonsDown = PAD_ButtonsDown(0);

    if (buttonsDown & PAD_BUTTON_START) {
      exit(0);
    }
  }
}
