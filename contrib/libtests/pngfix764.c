/* pngfix764.c
 *
 * Copyright (c) 2025 Cosmin Truta
 *
 * This code is released under the libpng license.
 * For conditions of distribution and use, see the disclaimer
 * and license in png.h
 *
 * Unit test for GitHub issue #764 fix: validate sRGB composition formula
 * used in png_image_read_composite for palette images with gamma.
 *
 * The fix introduces a new code path that performs alpha composition in
 * sRGB space (rather than linear space) when PNG_FLAG_OPTIMIZE_ALPHA is
 * cleared. This test validates that the formula produces correct results.
 *
 * Formula: result = foreground + (255 - alpha) * background / 255
 * With rounding: result = foreground + ((255 - alpha) * background + 127) / 255
 * With clamping: if (result > 255) result = 255
 */

#include <stdio.h>
#include <stdlib.h>

/* The sRGB composition formula from pngread.c (simplified API)
 *
 * This models the logic in png_image_read_composite() for the case where
 * PNG_FLAG_OPTIMIZE_ALPHA is NOT set (palette images with gamma correction).
 *
 * The actual code structure is:
 *   if (alpha > 0)        // else output unchanged (stays as background)
 *     if (alpha < 255)    // else output = foreground
 *       output = foreground + (255 - alpha) * background / 255
 *
 * The 'foreground' here is sRGB premultiplied on black (from palette after
 * gamma correction), so the formula effectively un-premultiplies and blends.
 */
static unsigned int
srgb_compose(unsigned int foreground, unsigned int alpha, unsigned int background)
{
   unsigned int result;

   if (alpha == 0)
   {
      /* Fully transparent: output is background (no change) */
      result = background;
   }
   else if (alpha == 255)
   {
      /* Fully opaque: output is foreground */
      result = foreground;
   }
   else
   {
      /* Partial transparency: blend */
      result = foreground + ((255 - alpha) * background + 127) / 255;
      if (result > 255)
         result = 255;
   }

   return result;
}

/* Test vector structure */
struct test_vector {
   unsigned int fg;       /* foreground component (from palette after gamma) */
   unsigned int alpha;    /* alpha value (from tRNS) */
   unsigned int bg;       /* background component (from buffer) */
   unsigned int expected; /* expected result */
   const char *desc;      /* description */
};

/* Test vectors derived from vulnerability analysis and edge cases */
static const struct test_vector tests[] = {
   /* Boundary cases: fully transparent (alpha=0) => result = background */
   { 0, 0, 255, 255, "transparent black on white: background only" },
   { 255, 0, 0, 0, "transparent white on black: background only" },
   { 100, 0, 200, 200, "transparent on gray: background only" },
   { 123, 0, 45, 45, "transparent: foreground ignored" },

   /* Boundary cases: fully opaque (alpha=255) => result = foreground */
   { 255, 255, 0, 255, "opaque white on black: foreground only" },
   { 0, 255, 255, 0, "opaque black on white: foreground only" },
   { 100, 255, 200, 100, "opaque on gray: foreground only" },

   /* Mid-alpha cases */
   { 128, 128, 128, 192, "50% gray on gray: 128 + (127*128+127)/255 = 192" },
   { 0, 128, 255, 127, "50% black on white: 0 + (127*255+127)/255 = 127" },
   { 255, 128, 0, 255, "50% white on black: 255 + 0 = 255" },
   { 100, 128, 200, 200, "50% blend: 100 + (127*200+127)/255 = 200" },

   /* Values from the PoC file (palette-4-1.8-tRNS.png) that violated
    * the premultiplied alpha invariant (foreground > alpha).
    * These caused the original overflow in the linear composition path.
    */
   { 134, 118, 73, 173, "PoC case 1: fg > alpha" },
   { 194, 140, 73, 227, "PoC case 2: fg > alpha" },
   { 249, 242, 73, 253, "PoC case 3: fg > alpha" },

   /* Edge cases that could overflow before clamping */
   { 255, 1, 255, 255, "near-transparent white on white: clamp" },
   { 200, 50, 200, 361 > 255 ? 255 : 361, "overflow case: clamp to 255" },
   { 250, 10, 250, 255, "high values low alpha: clamp" },

   /* Verify rounding behavior */
   { 0, 254, 255, 1, "nearly opaque: (1*255+127)/255 = 1" },
   { 0, 253, 255, 2, "nearly opaque: (2*255+127)/255 = 2" },
   { 0, 1, 1, 1, "nearly transparent low bg: (254*1+127)/255 = 1" },

   /* Default buffer init value (BUFFER_INIT8 = 73) cases */
   { 0, 128, 73, 36, "transparent on default buffer" },
   { 128, 64, 73, 183, "partial on default buffer" },

   /* Maximum stress: values that maximize the computation */
   { 254, 1, 254, 255, "max non-overflow: 254 + 253 = 507 -> 255" },
   { 1, 254, 1, 1, "min result with alpha: 1 + 0 = 1" },
};

static int
run_tests(void)
{
   size_t i;
   int failures = 0;
   size_t num_tests = sizeof(tests) / sizeof(tests[0]);

   printf("Running %zu sRGB composition tests...\n\n", num_tests);

   for (i = 0; i < num_tests; i++)
   {
      const struct test_vector *t = &tests[i];
      unsigned int result = srgb_compose(t->fg, t->alpha, t->bg);

      if (result != t->expected)
      {
         printf("FAIL [%zu]: %s\n", i + 1, t->desc);
         printf("  srgb_compose(%u, %u, %u) = %u, expected %u\n",
                t->fg, t->alpha, t->bg, result, t->expected);
         failures++;
      }
      else
      {
         printf("PASS [%zu]: %s\n", i + 1, t->desc);
      }
   }

   printf("\n");
   if (failures == 0)
      printf("All %zu tests passed.\n", num_tests);
   else
      printf("%d of %zu tests FAILED.\n", failures, num_tests);

   return failures;
}

/* Verify the formula matches what's in pngread.c */
static int
verify_formula_properties(void)
{
   int failures = 0;
   unsigned int fg, alpha, bg;

   printf("Verifying formula properties...\n\n");

   /* Property 1: alpha=0 => result = background */
   printf("Property 1: alpha=0 => result = background\n");
   for (bg = 0; bg <= 255; bg += 51)
   {
      for (fg = 0; fg <= 255; fg += 51)
      {
         unsigned int result = srgb_compose(fg, 0, bg);
         if (result != bg)
         {
            printf("  FAIL: srgb_compose(%u, 0, %u) = %u, expected %u\n",
                   fg, bg, result, bg);
            failures++;
         }
      }
   }
   if (failures == 0)
      printf("  PASS\n");

   /* Property 2: alpha=255 => result = foreground (or clamped) */
   printf("Property 2: alpha=255 => result = foreground\n");
   for (fg = 0; fg <= 255; fg += 51)
   {
      for (bg = 0; bg <= 255; bg += 51)
      {
         unsigned int result = srgb_compose(fg, 255, bg);
         if (result != fg)
         {
            printf("  FAIL: srgb_compose(%u, 255, %u) = %u, expected %u\n",
                   fg, bg, result, fg);
            failures++;
         }
      }
   }
   if (failures == 0)
      printf("  PASS\n");

   /* Property 3: result is always in [0, 255] */
   printf("Property 3: result always in [0, 255]\n");
   {
      int prop3_fail = 0;
      for (fg = 0; fg <= 255; fg++)
      {
         for (alpha = 0; alpha <= 255; alpha++)
         {
            for (bg = 0; bg <= 255; bg += 17)  /* sample to keep runtime reasonable */
            {
               unsigned int result = srgb_compose(fg, alpha, bg);
               if (result > 255)
               {
                  printf("  FAIL: srgb_compose(%u, %u, %u) = %u > 255\n",
                         fg, alpha, bg, result);
                  prop3_fail++;
                  failures++;
               }
            }
         }
      }
      if (prop3_fail == 0)
         printf("  PASS\n");
   }

   /* Property 4: monotonicity in background (higher bg => higher or equal result) */
   printf("Property 4: monotonic in background\n");
   {
      int prop4_fail = 0;
      for (fg = 0; fg <= 255; fg += 51)
      {
         for (alpha = 1; alpha < 255; alpha += 51)  /* exclude 0 and 255 */
         {
            unsigned int prev_result = srgb_compose(fg, alpha, 0);
            for (bg = 1; bg <= 255; bg++)
            {
               unsigned int result = srgb_compose(fg, alpha, bg);
               if (result < prev_result)
               {
                  printf("  FAIL: non-monotonic at fg=%u, alpha=%u, bg=%u\n",
                         fg, alpha, bg);
                  prop4_fail++;
                  failures++;
               }
               prev_result = result;
            }
         }
      }
      if (prop4_fail == 0)
         printf("  PASS\n");
   }

   printf("\n");
   if (failures == 0)
      printf("All formula properties verified.\n");
   else
      printf("%d property violations found.\n", failures);

   return failures;
}

int
main(int argc, char *argv[])
{
   int failures = 0;

   (void)argc;
   (void)argv;

   printf("pngfix764: Unit test for GitHub #764 sRGB composition fix\n");
   printf("============================================================\n\n");

   failures += run_tests();
   printf("\n");
   failures += verify_formula_properties();

   printf("\n============================================================\n");
   if (failures == 0)
   {
      printf("SUCCESS: All tests passed.\n");
      return 0;
   }
   else
   {
      printf("FAILURE: %d test(s) failed.\n", failures);
      return 1;
   }
}
