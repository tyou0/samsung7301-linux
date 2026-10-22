/*
 * analyze-frames.c - Per frame feature and match diagnostic for the Samsung
 * 04e8:7301 fingerprint sensor.
 *
 * Purpose
 * -------
 * analyze-matching.c measures three single frames and analyze-swipe.c
 * assembles the frames of a swipe into a larger image.  Both of them leave
 * one question open: does a LATE or SHIFTED frame of a capture, taken on its
 * own, already hold enough real minutiae to be matched?  The question is
 * asked before another capture is requested: when a single late frame of a
 * swipe reaches the score of 40, the matcher itself works and only the
 * assembly of the swipe is missing; when no single frame ever holds ten
 * minutiae, no amount of further touching will help.
 *
 * What is measured
 * ----------------
 * The input is exactly two sequences of thirty two raw 160x160 grayscale
 * frames, 2 * 32 * 25600 = 1638400 bytes on stdin.  The two sequences are
 * two independent captures of the same finger; their pixels are kept apart
 * at all times, and they are deliberately compared with each other below.
 *
 * EVERY one of the sixty four frames is measured on its own:
 *
 *   - the frame is contrast stretched on its own (min..max -> 0..255),
 *   - the stretched frame is turned into the pixels of the variant,
 *   - the minutiae are detected with libfprint (fp_image_detect_minutiae()),
 *   - the minutiae are converted into XYT points exactly as libfprint's
 *     fpi-print.c does it (lfs2nist_minutia_XYT(), the theta correction and
 *     the sort_x_y order),
 *   - the Bozorth matcher computes the SELF score of the frame, that is the
 *     score of the frame against itself.
 *
 * The pixels of the frames are never mixed, the first frame is never
 * reused for the other frames and no frame is ever copied from one
 * sequence into the other: every frame keeps its own pixels, its own count
 * of minutiae and its own score.
 *
 * The variants are the native frame (160), the manual bilinear enlargement
 * to 240 and the exact resize of the driver (fpi_image_resize(), a8 pixels,
 * a scale of two, a bilinear filter and no border) to 320, once normal and
 * once with the colours inverted.  The ppmm of a variant is
 * 19.69 * dimension / 160, exactly as the driver computes it.
 *
 * The report
 * ----------
 * For every variant and for every sequence:
 *
 *   - the smallest and the largest count of minutiae of the thirty two
 *     frames and the frames that hold at least ten minutiae, which is the
 *     minimum the matcher needs,
 *   - the frames whose self score reaches 40 and the three highest self
 *     scores with the index of the frame and its count of minutiae.
 *
 * Then the matcher is asked the question that matters: the score of EVERY
 * one of the 32 * 32 pairs of frames that are taken from DIFFERENT
 * sequences.  This cross-sequence comparison is deliberate, it is the point
 * of the tool.  A frame is never compared with itself and the same frame of
 * a sequence is never used twice in a pair.  The five highest cross scores
 * are printed with the indices and the counts of both frames and the
 * number of pairs that reach the threshold of 40 is printed.
 *
 * The self score only says how consistent a frame is with itself.  Only the
 * cross score can be compared with the threshold of 40.  The matcher itself
 * is not changed, the minimum of ten minutiae is not changed and no feature
 * is invented: a frame that holds no minutiae at all (a flat frame, or a
 * frame in which the detection found nothing) has a count of 0 and a self
 * score of 0, and that is a measurement and not a failure.  An error of the
 * detection that is not "no minutiae" is reported on stderr, the frame keeps
 * a count of 0 and the tool continues with the other frames.
 *
 * No pixel, no coordinate of a minutia, no hash and no template is ever
 * written to the output or to a file.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o /tmp/opencode/analyze-frames tools/analyze-frames.c \
 *      -I/tmp/opencode/libfprint-upstream/libfprint \
 *      -I/tmp/opencode/libfprint-upstream/libfprint/nbis/include \
 *      -I/tmp/opencode/libfprint-upstream/libfprint/nbis/libfprint-include \
 *      -I/tmp/opencode/libfprint-upstream/build \
 *      $(pkg-config --cflags --libs libfprint-2 gio-2.0 pixman-1) \
 *      /tmp/opencode/libfprint-upstream/build/libfprint/libnbis.a \
 *      -lm -lpthread
 *
 * Usage:
 *   /tmp/opencode/analyze-frames < frames.bin
 */

#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The static helpers of analyze-matching.c are reused unchanged: the
 * contrast stretch, the bilinear resize, the exact resize of the driver,
 * the image builder, the minutiae detection and the conversion into XYT
 * points.  Its main() is renamed so that this file can provide its own;
 * the helpers that this tool does not need are therefore unused, which is
 * not a warning here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#define main matching_main
#include "analyze-matching.c"
#undef main
#pragma GCC diagnostic pop

/* ------------------------------------------------------------------------- */

/* The two sequences of thirty two frames, the input is therefore
 * 2 * 32 * 25600 = 1638400 bytes. */
#define SEQ_COUNT      2
#define SEQ_LEN        32
#define FRAME_TOTAL    (SEQ_COUNT * SEQ_LEN)
#define INPUT_BYTES    (FRAME_TOTAL * FRAME_SIZE)

/* The matcher needs at least this many minutiae and it compares the score
 * with this threshold.  Neither of them is changed by this tool. */
#define MIN_MINUTIAE   10
#define MATCH_THRESHOLD 40

/* How many of the highest scores are listed. */
#define TOP_SELF       3
#define TOP_CROSS      5

/* ------------------------------------------------------------------------- */

/* The variants that are measured, in this order.  The struct and the
 * builder of the pixels are the ones of analyze-matching.c. */
static const Variant frame_variants[] = {
  { "native-160-normal",      V_BILINEAR, 160, FALSE },
  { "bilinear-240-normal",    V_BILINEAR, 240, FALSE },
  { "pixman-2x-320-normal",   V_PIXMAN,   320, FALSE },
  { "pixman-2x-320-inverted", V_PIXMAN,   320, TRUE  },
};

/* Everything that is measured for one variant: the XYT points, the count of
 * minutiae and the self score of every frame of every sequence.  The frames
 * of the two sequences are kept apart: xyt[0] and xyt[1] never share a
 * frame. */
typedef struct
{
  struct xyt_struct xyt[SEQ_COUNT][SEQ_LEN];
  guint            counts[SEQ_COUNT][SEQ_LEN];
  int              self_scores[SEQ_COUNT][SEQ_LEN];
} VariantData;

/* One frame with its score and its count of minutiae. */
typedef struct
{
  int   score;
  int   index;   /* the index of the frame inside its sequence */
  guint count;
} FrameScore;

/* One pair of frames of two different sequences with its score. */
typedef struct
{
  int   score;
  int   frame_a; /* the index of the frame in the first sequence  */
  int   frame_b; /* the index of the frame in the second sequence */
  guint count_a;
  guint count_b;
} CrossScore;

/* ------------------------------------------------------------------------- */

/* Write the indices of the frames into a string, separated by commas.  Only
 * the indices are written, never a coordinate of a feature. */
static void
format_indices (const int *indices,
                int         count,
                char       *out,
                size_t      out_size)
{
  size_t used = 0;

  if (out_size == 0)
    return;

  out[0] = '\0';

  for (int i = 0; i < count; i++)
    {
      int written;

      if (used >= out_size)
        break;

      written = snprintf (out + used, out_size - used, "%s%d",
                          i > 0 ? ", " : "", indices[i]);
      if (written < 0 || (size_t) written >= out_size - used)
        break;

      used += (size_t) written;
    }
}

/* The highest self scores of one sequence, with the index of the frame and
 * its count of minutiae.  A tie is won by the frame with the smaller
 * index. */
static void
top_self_scores (const VariantData *data,
                 int               seq,
                 FrameScore       *out,
                 int               top_n)
{
  gboolean taken[SEQ_LEN];

  memset (taken, 0, sizeof (taken));

  for (int n = 0; n < top_n; n++)
    {
      int best = -1;

      for (int f = 0; f < SEQ_LEN; f++)
        {
          if (taken[f])
            continue;

          if (best < 0 || data->self_scores[seq][f] > data->self_scores[seq][best])
            best = f;
        }

      if (best < 0)
        {
          out[n].score = 0;
          out[n].index = -1;
          out[n].count = 0;
          continue;
        }

      taken[best] = TRUE;
      out[n].score = data->self_scores[seq][best];
      out[n].index = best;
      out[n].count = data->counts[seq][best];
    }
}

/* The highest cross scores of the 32 * 32 pairs of the two sequences, with
 * the indices and the counts of both frames.  The pairs are the ones of
 * two DIFFERENT sequences, so a frame is never compared with itself. */
static void
top_cross_scores (const int   *pair_scores,
                  const VariantData *data,
                  CrossScore  *out,
                  int          top_n)
{
  gboolean taken[SEQ_LEN * SEQ_LEN];

  memset (taken, 0, sizeof (taken));

  for (int n = 0; n < top_n; n++)
    {
      int best = -1;

      for (int p = 0; p < SEQ_LEN * SEQ_LEN; p++)
        {
          if (taken[p])
            continue;

          if (best < 0 || pair_scores[p] > pair_scores[best])
            best = p;
        }

      if (best < 0)
        {
          out[n].score = 0;
          out[n].frame_a = -1;
          out[n].frame_b = -1;
          out[n].count_a = 0;
          out[n].count_b = 0;
          continue;
        }

      taken[best] = TRUE;
      out[n].score = pair_scores[best];
      out[n].frame_a = best / SEQ_LEN;
      out[n].frame_b = best % SEQ_LEN;
      out[n].count_a = data->counts[0][out[n].frame_a];
      out[n].count_b = data->counts[1][out[n].frame_b];
    }
}

/* ------------------------------------------------------------------------- */

int
main (void)
{
  guint8 *input;
  guint8 *stretched[FRAME_TOTAL];
  VariantData *data;
  int pair_scores[SEQ_LEN * SEQ_LEN];
  int failures = 0;

  input = g_try_malloc (INPUT_BYTES);
  if (!input)
    {
      fprintf (stderr, "error: out of memory\n");
      return 1;
    }

  if (fread (input, 1, INPUT_BYTES, stdin) != (size_t) INPUT_BYTES)
    {
      fprintf (stderr, "error: expected exactly %d bytes on stdin\n",
               INPUT_BYTES);
      g_free (input);
      return 1;
    }

  {
    guint8 extra;

    if (fread (&extra, 1, 1, stdin) != 0)
      {
        fprintf (stderr, "error: more than %d bytes on stdin\n", INPUT_BYTES);
        g_free (input);
        return 1;
      }
  }

  /* Every frame is stretched on its own; the sixty four frames stay
   * independent of each other. */
  for (int i = 0; i < FRAME_TOTAL; i++)
    stretched[i] = contrast_stretch (input + (gsize) i * FRAME_SIZE, FRAME_SIZE);
  g_free (input);

  data = g_try_new0 (VariantData, 1);
  if (!data)
    {
      fprintf (stderr, "error: out of memory\n");
      for (int i = 0; i < FRAME_TOTAL; i++)
        g_free (stretched[i]);
      return 1;
    }

  printf ("analyze-frames: %d sequences of %d frames of %dx%d (%d bytes)\n",
          SEQ_COUNT, SEQ_LEN, FRAME_WIDTH, FRAME_HEIGHT, INPUT_BYTES);
  printf ("matcher: minimum %d minutiae per frame, match threshold %d\n",
          MIN_MINUTIAE, MATCH_THRESHOLD);
  printf ("note: the self score only says how consistent a frame is with itself;\n"
          "      only the cross score can be compared with the threshold of %d.\n",
          MATCH_THRESHOLD);

  for (size_t v = 0; v < G_N_ELEMENTS (frame_variants); v++)
    {
      const Variant *variant = &frame_variants[v];

      memset (data->xyt, 0, sizeof (data->xyt));
      memset (data->counts, 0, sizeof (data->counts));
      memset (data->self_scores, 0, sizeof (data->self_scores));
      memset (pair_scores, 0, sizeof (pair_scores));

      /* Every one of the sixty four frames is measured on its own. */
      for (int s = 0; s < SEQ_COUNT; s++)
        {
          for (int f = 0; f < SEQ_LEN; f++)
            {
              int index = s * SEQ_LEN + f;
              guint8 *pixels;
              int dimension = variant->dimension;
              FpImage *image;
              GError *error = NULL;
              guint count = 0;
              int probe_len;

              pixels = variant_data (variant, stretched[index], &dimension);
              image = make_image (pixels, dimension, dimension,
                                  PPMM_NATIVE * dimension / (gdouble) FRAME_WIDTH,
                                  variant->inverted);
              g_free (pixels);

              if (detect_frame_minutiae (image, &count, &error) != 0)
                {
                  if (is_no_minutiae_error (error))
                    {
                      /* A frame without any minutiae is a measurement of
                       * zero and not a failure of the tool. */
                      g_clear_error (&error);
                      count = 0;
                    }
                  else
                    {
                      fflush (stdout);
                      fprintf (stderr, "error: %s: sequence %d frame %d: %s\n",
                               variant->name, s, f,
                               error ? error->message
                                     : "minutiae detection failed");
                      g_clear_error (&error);
                      count = 0;
                      failures++;
                    }
                }

              data->counts[s][f] = count;
              minutiae_to_xyt (fp_image_get_minutiae (image), dimension,
                               dimension, &data->xyt[s][f]);
              g_object_unref (image);

              /* The self score of the frame, exactly as libfprint's
               * fpi_print_bz3_match() computes the score of a print. */
              probe_len = bozorth_probe_init (&data->xyt[s][f]);
              data->self_scores[s][f] =
                bozorth_to_gallery (probe_len, &data->xyt[s][f],
                                    &data->xyt[s][f]);
            }
        }

      printf ("\nvariant %s\n", variant->name);

      for (int s = 0; s < SEQ_COUNT; s++)
        {
          guint min_count = G_MAXUINT;
          guint max_count = 0;
          int good[SEQ_LEN];
          int good_count = 0;
          int hits[SEQ_LEN];
          int hit_count = 0;
          FrameScore top[TOP_SELF];
          char list[512];

          for (int f = 0; f < SEQ_LEN; f++)
            {
              guint count = data->counts[s][f];

              if (count < min_count)
                min_count = count;
              if (count > max_count)
                max_count = count;

              if (count >= MIN_MINUTIAE)
                good[good_count++] = f;

              if (data->self_scores[s][f] >= MATCH_THRESHOLD)
                hits[hit_count++] = f;
            }

          top_self_scores (data, s, top, TOP_SELF);

          printf ("  sequence %d (frames 0..%d)\n", s, SEQ_LEN - 1);
          printf ("    minutiae per frame: min %u, max %u\n",
                  min_count, max_count);

          format_indices (good, good_count, list, sizeof (list));
          printf ("    frames with >= %d minutiae: %d",
                  MIN_MINUTIAE, good_count);
          if (good_count > 0)
            printf (" (frames %s)", list);
          printf ("\n");

          format_indices (hits, hit_count, list, sizeof (list));
          printf ("    frames with self score >= %d: %d",
                  MATCH_THRESHOLD, hit_count);
          if (hit_count > 0)
            printf (" (frames %s)", list);
          printf ("\n");

          printf ("    highest self scores:\n");
          for (int n = 0; n < TOP_SELF; n++)
            {
              if (top[n].index < 0)
                continue;

              printf ("      %3d  frame %2d  (%u minutiae)\n",
                      top[n].score, top[n].index, top[n].count);
            }
        }

      /* The score of every one of the 32 * 32 pairs of two DIFFERENT
       * sequences.  A frame is never compared with itself. */
      for (int f = 0; f < SEQ_LEN; f++)
        {
          int probe_len = bozorth_probe_init (&data->xyt[0][f]);

          for (int g = 0; g < SEQ_LEN; g++)
            {
              pair_scores[f * SEQ_LEN + g] =
                bozorth_to_gallery (probe_len, &data->xyt[0][f],
                                    &data->xyt[1][g]);
            }
        }

      {
        int pairs = 0;
        CrossScore top[TOP_CROSS];

        for (int p = 0; p < SEQ_LEN * SEQ_LEN; p++)
          {
            if (pair_scores[p] >= MATCH_THRESHOLD)
              pairs++;
          }

        top_cross_scores (pair_scores, data, top, TOP_CROSS);

        printf ("  cross pairs (sequence 0 x sequence 1, never the same frame): %d\n",
                SEQ_LEN * SEQ_LEN);
        printf ("    pairs with score >= %d (match threshold %d): %d\n",
                MATCH_THRESHOLD, MATCH_THRESHOLD, pairs);
        printf ("    highest cross scores:\n");
        for (int n = 0; n < TOP_CROSS; n++)
          {
            if (top[n].frame_a < 0)
              continue;

            printf ("      %3d  sequence 0 frame %2d (%u minutiae)"
                    "  -  sequence 1 frame %2d (%u minutiae)\n",
                    top[n].score, top[n].frame_a, top[n].count_a,
                    top[n].frame_b, top[n].count_b);
          }
      }

      fflush (stdout);
    }

  if (failures > 0)
    printf ("\nnote: %d detection errors were reported on stderr; the frames\n"
            "      concerned have a count of 0 and a self score of 0.\n",
            failures);

  g_free (data);

  for (int i = 0; i < FRAME_TOTAL; i++)
    g_free (stretched[i]);

  return failures ? 1 : 0;
}
