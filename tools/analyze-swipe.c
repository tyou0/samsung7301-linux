/*
 * analyze-swipe.c - Swipe assembly and matching diagnostic for the Samsung
 * 04e8:7301 fingerprint sensor.
 *
 * Purpose
 * -------
 * A single press of this sensor only covers a small patch of the finger:
 * the enrolment samples hold 10..13 minutiae and the Bozorth score between
 * two such samples stays far below the threshold of 40 that libfprint
 * compares against.  Counting the features is therefore not enough, and the
 * single press variants of analyze-matching.c (native, intermediate, 2x,
 * with and without a contrast stretch) all fail to reach the threshold.
 *
 * A swipe is different: the finger moves over the sensor, so the frames of
 * one swipe cover a much larger area of the finger and should hold many
 * more minutiae.  This tool assembles the frames of a swipe into one image
 * and measures what the matcher scores between two independent swipes of
 * the same finger.  The question is narrow: does the REAL larger finger
 * area reach the score of 40?
 *
 * How the frames are assembled
 * ----------------------------
 * Two adjacent frames are registered against each other with a zero mean
 * normalised cross correlation (NCC) over the RAW pixels of their overlap:
 *
 *   - a coarse grid of dx in [-40, 40] and dy in [-48, 48] in steps of 4,
 *     every candidate scored on every fourth pixel of the overlap,
 *   - a refinement of +-3 around the best coarse offset in steps of 1,
 *     scored on every second pixel of the overlap.
 *
 * The offset with the highest NCC wins.  A near tie is decided in favour
 * of the smaller displacement, so two identical frames are registered with
 * a displacement of exactly zero.  A pair is rejected when the best NCC is
 * below 0.75, or when the maximum was found at the boundary of the search
 * range, because then the true motion is probably larger than the search.
 *
 * The offsets are the physically estimated integer translations between the
 * frames; nothing is invented and no frame is duplicated.  When the frames
 * are compared as A(x,y) and B(x+dx, y+dy), the origin of B in the canvas
 * of the sweep is the origin of A minus (dx, dy); the origins of the
 * frames are accumulated along the sweep.
 *
 * Which part of the swipe is used
 * -------------------------------
 * A capture does not start and end with motion: the finger settles, it may
 * stand still and it may leave the sensor.  Such a stretch shows up as a
 * chain of pairs that are either rejected (a correlation below 0.75 or a
 * maximum at the border of the search) or that are accepted with an
 * offset of zero.  Assembling all of the frames would glue those
 * stretches to the moving ones, and the result would neither be a real
 * picture of a swipe nor larger than a single frame.
 *
 * ALL thirty one pairs of a sweep are registered and all of them are
 * reported.  The frames are then split into the chains of consecutive
 * accepted pairs; a rejected pair ends a chain and two chains are never
 * joined with each other.
 *
 * EVERY contiguous stretch of a chain that spans at least eight frames is
 * a candidate, not only the maximal stretch of the chain.  A chain that
 * drifts far can grow a canvas that no longer fits into 640x1024, and
 * then the maximal stretch alone would hide the shorter stretches of the
 * same swipe that do fit and that hold a real area.  The candidates are
 * compared by the y travel that their own measured offsets describe; the
 * largest travel wins and a tie is won by the candidate with more frames.
 * A candidate is only used when its canvas is at most 640x1024 and when
 * its y travel is at least 20 pixels.
 *
 * The origins are built INSIDE the chosen candidate, from the offsets of
 * the accepted pairs of that candidate only, and the assembly, the
 * blending and the measurement use the frames of that candidate only.
 * The offsets of a rejected pair are never used to connect two frames, a
 * rejected pair never joins two segments and the segments are never
 * joined with each other.  The y travel of a candidate is the travel that
 * its own offsets describe, so it is the real travel of the finger and
 * not a number that was tuned.
 *
 * The frames are then blended into one canvas.  The canvas is the full
 * bounding UNION of the frames of the candidate: it reaches from the
 * smallest to the largest x origin and from the smallest to the largest
 * y origin, every frame adding its own 160x160 window to it.  An earlier
 * version cropped the canvas to the x range that ALL frames cover, which
 * threw away the pixels that the finger really swept over: a swipe of
 * 55 pixels in x and of 60 pixels in y lost more than half of its area
 * and ended up smaller than a single frame.  The union keeps every pixel
 * that was really captured.
 *
 * A canvas pixel is the mean of the RAW pixels of the frames that cover
 * it.  The loop of every frame is clamped to the intersection of that
 * frame with the canvas, so that only pixels that were really captured
 * are ever blended.  A canvas pixel that no frame covers is UNKNOWN: it
 * is filled with 255 and it is marked as such in a separate coverage
 * mask that is kept with the image.  No ridge is invented there.
 *
 * What is measured
 * ----------------
 * The assembled images are analysed exactly like analyze-matching.c does:
 * the image is contrast stretched, scaled by 1.0, 1.5 and 2.0 with the same
 * bilinear interpolation, the minutiae are detected with libfprint and
 * converted into XYT points, and the Bozorth matcher computes the self
 * score of each sweep and the genuine score between the two sweeps.  The
 * matcher is not changed and the resolution stays 19.69 * scale.
 *
 * The unknown padding of the canvas must not supply the matcher with
 * features.  Every detected minutia is therefore checked against the
 * coverage mask before it is converted into a point: a minutia is kept
 * when the whole square of eight NATIVE pixels around it lies inside the
 * canvas and when every pixel of that square was really captured.  A
 * minutia in the border of the captured area or in an uncovered part of
 * the canvas is dropped.  The check is done on a temporary shallow array
 * of the detected minutiae; the array that libfprint owns is never
 * changed.  The number of detected minutiae and the number of interior
 * minutiae are both reported, and only the interior ones are used for the
 * self and for the genuine score.  A variant only scales the image, so
 * a scaled minutia is mapped back to the canvas by dividing its
 * coordinates by the scale of the variant; the inversion does not move
 * anything at all.
 *
 * The self score only says how consistent one sweep is with itself.  It is
 * NOT evidence that the matcher works; only the genuine score between the
 * two independent sweeps can be compared with the threshold of 40.
 *
 * A sweep that was rejected holds no image and is therefore not measured at
 * all.  Such a sweep is reported with a count of minutiae of 0 and a self
 * score of -1, and both of them mean that the value is unavailable and not
 * that it is zero.  The genuine score needs both sweeps, so it is reported
 * as "unavailable" when one of the two sweeps was rejected; it is never
 * reported as a score of 0, which could be read as a match.  The tool only
 * stops before the measurement when BOTH sweeps were rejected, so that one
 * good sweep can still be diagnosed on its own.
 *
 * Input and output
 * ----------------
 * The input is exactly two sweeps of thirty two raw 160x160 grayscale
 * frames, 2 * 32 * 25600 = 1638400 bytes on stdin.  Only the numbers below
 * are printed; no pixel, no coordinate and no template is ever written to the
 * output or to a file.
 *
 * The report
 * ----------
 * Before the frames are assembled, every sweep is measured on its own:
 *
 *   - the smallest, the largest, the mean and the standard deviation of
 *     the raw pixels of each of the thirty two frames,
 *   - the registration of all thirty one adjacent pairs, computed on the
 *     ORIGINAL frames before any geometry is done.  Every pair is
 *     reported, also the rejected ones, with the offset (dx, dy) that was
 *     found, the correlation of that offset, the correlation of the pair
 *     without any offset and whether the pair was accepted.
 *
 * The offsets are the shifts between the frames of the capture.  They are
 * NOT the coordinates of minutiae and they say nothing about the finger.
 * The total travel in x and in y is printed when the chain of the thirty
 * one pairs is valid.
 *
 * The stretch of the assembled image uses the pixels that were really
 * captured only, so that the padding of 255 does not take the contrast
 * away from the real pixels of the swipe.
 *
 * The stretch of frames that the sweep is assembled from is printed with
 * its frame range, its worst correlation, its travel in x and in y, the
 * size of its canvas and the number of its pixels that were really
 * captured.
 *
 * The thresholds and the assembly itself are unchanged: a pair is still
 * rejected when its best correlation is below 0.75 or when the maximum was
 * found at the boundary of the search, and a sweep is still rejected when
 * the canvas is too large or when the total y travel is below 20.  A
 * rejected pair now ends a chain instead of rejecting the whole sweep,
 * but it still hides nothing: every one of the thirty one pairs is
 * reported.  The rule that the x range of all the frames has to be at
 * least 80 pixels wide is gone: it only ever threw real pixels away, and
 * every adjacent pair overlaps by at least 112 pixels anyway.
 *
 * The table at the end holds the count of the detected minutiae, the
 * count of the interior minutiae and the self score of every sweep of
 * every variant and the genuine score of the variant.  A value that was
 * not measured is never shown as a number that could be read as a
 * measurement.
 *
 * Build:
 *   cc -O2 -Wall -Wextra -o /tmp/opencode/analyze-swipe tools/analyze-swipe.c \
 *      -I/tmp/opencode/libfprint-upstream/libfprint \
 *      -I/tmp/opencode/libfprint-upstream/libfprint/nbis/include \
 *      -I/tmp/opencode/libfprint-upstream/libfprint/nbis/libfprint-include \
 *      -I/tmp/opencode/libfprint-upstream/build \
 *      $(pkg-config --cflags --libs libfprint-2 gio-2.0 pixman-1) \
 *      /tmp/opencode/libfprint-upstream/build/libfprint/libnbis.a \
 *      -lm -lpthread
 *
 * Usage:
 *   ./analyze-swipe < sweeps.bin
 */

#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The static helpers of analyze-matching.c are reused unchanged: the
 * bilinear resize, the image builder, the minutiae detection and the
 * conversion into XYT points.  The contrast stretch of a canvas that
 * holds unknown pixels is done here instead, because only the pixels
 * that were really captured may take part in it.  Its main() is renamed
 * so that this file can provide its own; the helpers that this tool does
 * not need are therefore unused, which is not a warning here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#define main matching_main
#include "analyze-matching.c"
#undef main
#pragma GCC diagnostic pop

/* ------------------------------------------------------------------------- */

/* One sweep: thirty two frames of 160x160 grayscale pixels. */
#define SWIPE_FRAME_W     160
#define SWIPE_FRAME_H     160
#define SWIPE_FRAME_BYTES (SWIPE_FRAME_W * SWIPE_FRAME_H)
#define SWIPE_FRAMES      32
#define SWIPE_COUNT       2
#define SWIPE_INPUT_BYTES (SWIPE_COUNT * SWIPE_FRAMES * SWIPE_FRAME_BYTES)

/* A stretch of consecutive accepted pairs has to span at least this many
 * frames; a shorter stretch cannot hold a real area. */
#define MIN_RUN_FRAMES    8

/* The registration of two adjacent frames: the coarse grid and the
 * refinement around its maximum. */
#define REG_COARSE_DX     40
#define REG_COARSE_DY     48
#define REG_COARSE_STEP   4
#define REG_COARSE_STRIDE 4
#define REG_REFINE_RADIUS 3
#define REG_REFINE_STRIDE 2

/* A pair is only accepted with a high correlation, and two correlations
 * that differ by less than this are a tie that the smaller displacement
 * wins. */
#define REG_NCC_MIN 0.75
#define REG_TIE_EPS 1e-6

/* The limits of the assembled canvas and of the area it adds. */
#define CANVAS_MAX_W 640
#define CANVAS_MAX_H 1024
#define TRAVEL_Y_MIN 20

/* A minutia is only used when the whole square of this radius around it
 * lies inside the canvas and was really captured.  The radius is counted
 * in NATIVE pixels of the canvas, not in the pixels of the variant. */
#define MINUTIA_RADIUS_NATIVE 8

/* ------------------------------------------------------------------------- */

/* The offset of one frame in the canvas of its sweep and the correlation
 * that produced it. */
typedef struct
{
  gboolean valid;
  int      dx;
  int      dy;
  double   ncc;
} Offset;

/* The result of the assembly of one sweep. */
typedef struct
{
  gboolean ok;         /* the sweep was assembled into an image       */
  char     reason[256]; /* why the sweep was rejected                  */
  double   min_ncc;    /* the worst correlation of all the pairs       */
  gboolean have_ncc;
  double   run_min_ncc; /* the worst correlation inside the chosen run */
  int      first_frame; /* the first frame of the chosen run           */
  int      last_frame;  /* the last frame of the chosen run            */
  int      travel_x;
  int      travel_y;
  int      width;
  int      height;
  guint8  *pixels;     /* the assembled image, owned                  */
  guint8  *mask;       /* the coverage of the canvas, owned           */
  gsize    covered;    /* the pixels of the canvas that were captured */
} Sweep;

/* A stretch of consecutive accepted pairs and the numbers that describe
 * it.  The stretch spans the frames first_pair .. last_pair + 1 and its
 * origins are built from the offsets of those pairs only. */
typedef struct
{
  gboolean valid;       /* the stretch may be assembled into an image    */
  int      first_pair;
  int      last_pair;
  int      first_frame;
  int      last_frame;
  int      frames;      /* the number of frames the stretch spans         */
  int      pairs;       /* the number of pairs the stretch holds         */
  double   min_ncc;     /* the worst correlation of the pairs of it      */
  int      travel_x;
  int      travel_y;
  int      min_x;
  int      max_x;
  int      min_y;
  int      max_y;
  int      canvas_w;    /* the width of the union of all its frames      */
  int      canvas_h;    /* the height of the union of all its frames     */
  char     reason[192]; /* why the stretch may not be assembled          */
} Run;

/* The variants of the assembled image that are measured. */
typedef struct
{
  const char *name;
  double      scale;
  gboolean    inverted;
} SwipeVariant;

static const SwipeVariant swipe_variants[] = {
  { "native-1.00",       1.00, FALSE },
  { "native-1.00-inv",   1.00, TRUE  },
  { "resized-1.50",      1.50, FALSE },
  { "resized-1.50-inv",  1.50, TRUE  },
  { "resized-2.00",      2.00, FALSE },
  { "resized-2.00-inv",  2.00, TRUE  },
};

/* ------------------------------------------------------------------------- */

/* The zero mean normalised cross correlation of the overlap of two frames.
 *
 * A(x, y) is compared with B(x + dx, y + dy); the overlap is the set of
 * (x, y) for which both samples exist.  Every stride'th pixel of the
 * overlap takes part in the correlation.  The result is -1 when the overlap
 * has no variation, because the correlation of a flat area is not defined.
 */
static double
frame_ncc (const guint8 *a,
           const guint8 *b,
           int           dx,
           int           dy,
           int           stride)
{
  int x0 = dx < 0 ? -dx : 0;
  int x1 = dx > 0 ? SWIPE_FRAME_W - 1 - dx : SWIPE_FRAME_W - 1;
  int y0 = dy < 0 ? -dy : 0;
  int y1 = dy > 0 ? SWIPE_FRAME_H - 1 - dy : SWIPE_FRAME_H - 1;
  double sum_a = 0.0;
  double sum_b = 0.0;
  double num = 0.0;
  double var_a = 0.0;
  double var_b = 0.0;
  double mean_a;
  double mean_b;
  int n = 0;

  for (int y = y0; y <= y1; y += stride)
    for (int x = x0; x <= x1; x += stride)
      {
        sum_a += a[y * SWIPE_FRAME_W + x];
        sum_b += b[(y + dy) * SWIPE_FRAME_W + (x + dx)];
        n++;
      }

  if (n < 16)
    return -1.0;

  mean_a = sum_a / n;
  mean_b = sum_b / n;

  /* The second pass uses the deviations from the means, so that the
   * correlation does not lose its precision in the subtraction. */
  for (int y = y0; y <= y1; y += stride)
    for (int x = x0; x <= x1; x += stride)
      {
        double va = a[y * SWIPE_FRAME_W + x] - mean_a;
        double vb = b[(y + dy) * SWIPE_FRAME_W + (x + dx)] - mean_b;

        num += va * vb;
        var_a += va * va;
        var_b += vb * vb;
      }

  if (var_a <= 1e-9 || var_b <= 1e-9)
    return -1.0;

  return num / sqrt (var_a * var_b);
}

/* True when the candidate is better than the current best.  A clearly
 * higher correlation always wins, a near tie is decided in favour of the
 * smaller displacement, so that two identical frames keep a displacement
 * of exactly zero. */
static gboolean
offset_is_better (double       ncc,
                  int          dx,
                  int          dy,
                  const Offset *best)
{
  int cand_disp;
  int best_disp;

  if (!best->valid)
    return TRUE;

  if (ncc > best->ncc + REG_TIE_EPS)
    return TRUE;
  if (ncc < best->ncc - REG_TIE_EPS)
    return FALSE;

  cand_disp = abs (dx) + abs (dy);
  best_disp = abs (best->dx) + abs (best->dy);

  if (cand_disp != best_disp)
    return cand_disp < best_disp;
  if (abs (dx) != abs (best->dx))
    return abs (dx) < abs (best->dx);

  return abs (dy) < abs (best->dy);
}

/* True when the offset sits at the border of the search range, which means
 * that the true motion is probably larger than the search. */
static gboolean
at_search_boundary (int value,
                    int limit)
{
  return value == limit || value == -limit;
}

/* Register two adjacent frames of a sweep.  Returns TRUE when the pair is
 * accepted, FALSE when it is rejected; the reason is then stored in
 * reason. */
static gboolean
register_pair (const guint8 *a,
               const guint8 *b,
               Offset        *result,
               char          *reason,
               size_t         reason_len)
{
  Offset coarse = { 0 };
  Offset fine = { 0 };
  int dx;
  int dy;

  /* The coarse grid over the whole search range. */
  for (dy = -REG_COARSE_DY; dy <= REG_COARSE_DY; dy += REG_COARSE_STEP)
    for (dx = -REG_COARSE_DX; dx <= REG_COARSE_DX; dx += REG_COARSE_STEP)
      {
        double ncc = frame_ncc (a, b, dx, dy, REG_COARSE_STRIDE);

        if (ncc < 0.0)
          continue;
        if (offset_is_better (ncc, dx, dy, &coarse))
          {
            coarse.valid = TRUE;
            coarse.dx = dx;
            coarse.dy = dy;
            coarse.ncc = ncc;
          }
      }

  if (!coarse.valid)
    {
      *result = coarse;
      snprintf (reason, reason_len, "the overlap of the two frames has no variation");
      return FALSE;
    }

  /* The refinement around the maximum of the coarse grid. */
  for (dy = MAX (-REG_COARSE_DY, coarse.dy - REG_REFINE_RADIUS);
       dy <= MIN (REG_COARSE_DY, coarse.dy + REG_REFINE_RADIUS); dy++)
    for (dx = MAX (-REG_COARSE_DX, coarse.dx - REG_REFINE_RADIUS);
         dx <= MIN (REG_COARSE_DX, coarse.dx + REG_REFINE_RADIUS); dx++)
      {
        double ncc = frame_ncc (a, b, dx, dy, REG_REFINE_STRIDE);

        if (ncc < 0.0)
          continue;
        if (offset_is_better (ncc, dx, dy, &fine))
          {
            fine.valid = TRUE;
            fine.dx = dx;
            fine.dy = dy;
            fine.ncc = ncc;
          }
      }

  if (!fine.valid)
    {
      /* The refinement found nothing usable; the coarse maximum is still
       * the best known offset and is reported as such. */
      *result = coarse;
      snprintf (reason, reason_len, "the overlap of the two frames has no variation");
      return FALSE;
    }

  /* The best offset is handed back BEFORE the acceptance tests below, so
   * that a rejected pair still reports the offset and the correlation that
   * were found.  The caller uses this only for the diagnostic. */
  *result = fine;

  if (fine.ncc < REG_NCC_MIN)
    {
      snprintf (reason, reason_len, "the best correlation %.4f is below %.2f",
                fine.ncc, REG_NCC_MIN);
      return FALSE;
    }

  if (at_search_boundary (fine.dx, REG_COARSE_DX) ||
      at_search_boundary (fine.dy, REG_COARSE_DY) ||
      at_search_boundary (coarse.dx, REG_COARSE_DX) ||
      at_search_boundary (coarse.dy, REG_COARSE_DY))
    {
      snprintf (reason, reason_len,
                "the maximum of the search was found at its boundary");
      return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------------- */

/* The statistics of one raw frame: the smallest and the largest pixel and
 * the standard deviation.  They show whether a frame carries a pattern at
 * all or only a flat level. */
static void
print_frame_stats (const guint8 *frame,
                   int           frame_index)
{
  double sum = 0.0;
  double sum_sq = 0.0;
  int min = 255;
  int max = 0;

  for (int i = 0; i < SWIPE_FRAME_BYTES; i++)
    {
      int value = frame[i];

      sum += value;
      sum_sq += (double) value * value;
      if (value < min)
        min = value;
      if (value > max)
        max = value;
    }

  {
    double mean = sum / SWIPE_FRAME_BYTES;
    double var = sum_sq / SWIPE_FRAME_BYTES - mean * mean;
    double stddev = var > 0.0 ? sqrt (var) : 0.0;

    printf ("  frame %2d  min %3d  max %3d  mean %6.2f  stddev %6.2f\n",
            frame_index, min, max, mean, stddev);
  }
}

/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */

/* The numbers of one stretch of consecutive accepted pairs.
 *
 * The origins of the frames of the stretch are built INSIDE the stretch,
 * from the offsets of the accepted pairs of that stretch only: the offset
 * of a rejected pair never connects two frames, a rejected pair never
 * joins two stretches and two stretches are never joined with each
 * other.  The travel of a stretch is therefore the travel that the
 * measured offsets describe; no motion is invented and nothing is tuned.
 *
 * The canvas of a stretch is the full bounding UNION of its frames: it
 * reaches from the smallest to the largest x origin and from the smallest
 * to the largest y origin, every frame adding its own 160x160 window to
 * it.  A stretch is usable when that canvas is at most 640x1024 and when
 * its y travel is at least 20 pixels. */
static void
evaluate_run (const Offset  *offsets,
              int            first_pair,
              int            last_pair,
              int           *origin_x,
              int           *origin_y,
              Run           *run)
{
  memset (run, 0, sizeof (*run));

  run->first_pair = first_pair;
  run->last_pair = last_pair;
  run->first_frame = first_pair;
  run->last_frame = last_pair + 1;
  run->frames = run->last_frame - run->first_frame + 1;
  run->pairs = last_pair - first_pair + 1;

  origin_x[run->first_frame] = 0;
  origin_y[run->first_frame] = 0;

  for (int f = run->first_frame + 1; f <= run->last_frame; f++)
    {
      /* B(x + dx, y + dy) is A(x, y), so the canvas origin of B is the
       * canvas origin of A minus the offset. */
      origin_x[f] = origin_x[f - 1] - offsets[f - 1].dx;
      origin_y[f] = origin_y[f - 1] - offsets[f - 1].dy;
    }

  run->min_x = run->max_x = origin_x[run->first_frame];
  run->min_y = run->max_y = origin_y[run->first_frame];

  for (int f = run->first_frame; f <= run->last_frame; f++)
    {
      if (origin_x[f] < run->min_x)
        run->min_x = origin_x[f];
      if (origin_x[f] > run->max_x)
        run->max_x = origin_x[f];
      if (origin_y[f] < run->min_y)
        run->min_y = origin_y[f];
      if (origin_y[f] > run->max_y)
        run->max_y = origin_y[f];
    }

  run->travel_x = run->max_x - run->min_x;
  run->travel_y = run->max_y - run->min_y;
  run->canvas_w = run->max_x + SWIPE_FRAME_W - run->min_x;
  run->canvas_h = run->max_y + SWIPE_FRAME_H - run->min_y;

  run->min_ncc = 2.0;
  for (int p = first_pair; p <= last_pair; p++)
    {
      if (offsets[p].ncc < run->min_ncc)
        run->min_ncc = offsets[p].ncc;
    }

  if (run->canvas_w > CANVAS_MAX_W || run->canvas_h > CANVAS_MAX_H)
    snprintf (run->reason, sizeof (run->reason),
              "the canvas of %dx%d is larger than %dx%d",
              run->canvas_w, run->canvas_h, CANVAS_MAX_W, CANVAS_MAX_H);
  else if (run->travel_y < TRAVEL_Y_MIN)
    snprintf (run->reason, sizeof (run->reason),
              "the total y travel of %d is below %d, no real area was added",
              run->travel_y, TRAVEL_Y_MIN);
  else
    run->valid = TRUE;
}

/* The better of two stretches: the one with the larger y travel, a tie is
 * won by the stretch with more frames. */
static gboolean
run_is_better (const Run *candidate,
               const Run *best)
{
  if (candidate->travel_y != best->travel_y)
    return candidate->travel_y > best->travel_y;

  return candidate->frames > best->frames;
}

/* ------------------------------------------------------------------------- */

/* Assemble the frames of one sweep into a single image.  The frames are
 * blended by the mean of the raw pixels that cover a pixel; the canvas is
 * the full bounding union of the frames of the chosen stretch and every
 * pixel that no frame covers is unknown.
 *
 * The work is done in three phases.  The first phase registers all thirty
 * one adjacent pairs on the ORIGINAL frames, before any geometry is done,
 * and reports every one of them, also the rejected ones.  The second phase
 * splits the frames into the chains of consecutive accepted pairs and
 * enumerates every stretch of at least eight frames inside those chains,
 * and picks the stretch with the largest y travel that fits into the
 * canvas limits.  The third phase builds the origins inside that stretch,
 * lays the frames of the stretch out and blends them.  Only the frames of
 * the chosen stretch are used. */
static gboolean
assemble_sweep (const guint8 *frames,
                int           sweep_index,
                Sweep        *sweep)
{
  Offset    offsets[SWIPE_FRAMES - 1];
  gboolean  accepted[SWIPE_FRAMES - 1];
  char      reasons[SWIPE_FRAMES - 1][128];
  int       origin_x[SWIPE_FRAMES];
  int       origin_y[SWIPE_FRAMES];
  Run       run;
  Run       best_candidate;
  Run       best_valid;
  gboolean  have_candidate = FALSE;
  gboolean  have_valid = FALSE;
  int       canvas_x0;
  int       canvas_x1;
  int       canvas_y0;
  int       canvas_y1;
  int       canvas_w;
  int       canvas_h;
  double   *sum;
  guint32  *count;
  guint8   *pixels;
  guint8   *mask;
  gsize     pixels_n;
  gsize     covered = 0;
  gboolean  all_accepted = TRUE;
  int       first_rejected = -1;
  int       accepted_pairs = 0;
  int       longest_run = 0;

  memset (sweep, 0, sizeof (*sweep));
  memset (&best_candidate, 0, sizeof (best_candidate));
  memset (&best_valid, 0, sizeof (best_valid));

  /* The statistics of the raw frames. */
  printf ("sweep %d: the statistics of the %d raw frames\n",
          sweep_index, SWIPE_FRAMES);
  for (int f = 0; f < SWIPE_FRAMES; f++)
    print_frame_stats (frames + f * SWIPE_FRAME_BYTES, f);

  /* The first phase: all thirty one adjacent pairs are registered on the
   * original frames and every one of them is reported.  The offsets are
   * the shifts between the frames of the capture; they are NOT
   * coordinates of minutiae and they say nothing about the finger. */
  printf ("sweep %d: the registration of the %d adjacent pairs"
          " (frame offsets, not minutiae coordinates)\n",
          sweep_index, SWIPE_FRAMES - 1);

  for (int f = 1; f < SWIPE_FRAMES; f++)
    {
      const guint8 *a = frames + (f - 1) * SWIPE_FRAME_BYTES;
      const guint8 *b = frames + f * SWIPE_FRAME_BYTES;
      char reason[128];
      double ncc_zero = frame_ncc (a, b, 0, 0, REG_REFINE_STRIDE);

      reason[0] = '\0';
      accepted[f - 1] = register_pair (a, b, &offsets[f - 1],
                                       reason, sizeof (reason));
      snprintf (reasons[f - 1], sizeof (reasons[f - 1]), "%s", reason);

      if (!accepted[f - 1] && first_rejected < 0)
        first_rejected = f - 1;
      if (!accepted[f - 1])
        all_accepted = FALSE;
      else
        accepted_pairs++;

      if (!offsets[f - 1].valid)
        printf ("  pair %2d/%2d  dx   ?  dy   ?  ncc   n/a  ncc@0,0 %6.4f  REJECT (%s)\n",
                f - 1, f, ncc_zero, reason);
      else
        printf ("  pair %2d/%2d  dx %3d  dy %3d  ncc %6.4f  ncc@0,0 %6.4f  %s%s%s\n",
                f - 1, f, offsets[f - 1].dx, offsets[f - 1].dy,
                offsets[f - 1].ncc, ncc_zero,
                accepted[f - 1] ? "ACCEPT" : "REJECT (",
                accepted[f - 1] ? "" : reason,
                accepted[f - 1] ? "" : ")");
    }

  /* The correlation of the whole sweep, the worst of the thirty one
   * pairs. */
  for (int f = 0; f < SWIPE_FRAMES - 1; f++)
    {
      if (!offsets[f].valid)
        continue;
      if (!sweep->have_ncc || offsets[f].ncc < sweep->min_ncc)
        sweep->min_ncc = offsets[f].ncc;
      sweep->have_ncc = TRUE;
    }

  /* The travel that the offsets describe, also when a pair was rejected.
   * This is a diagnostic only; the assembly uses it when every pair was
   * accepted. */
  {
    int acc_x = 0;
    int acc_y = 0;
    int b_min_x = 0;
    int b_max_x = 0;
    int b_min_y = 0;
    int b_max_y = 0;

    for (int f = 1; f < SWIPE_FRAMES; f++)
      {
        int dx = offsets[f - 1].valid ? offsets[f - 1].dx : 0;
        int dy = offsets[f - 1].valid ? offsets[f - 1].dy : 0;

        acc_x -= dx;
        acc_y -= dy;
        if (acc_x < b_min_x) b_min_x = acc_x;
        if (acc_x > b_max_x) b_max_x = acc_x;
        if (acc_y < b_min_y) b_min_y = acc_y;
        if (acc_y > b_max_y) b_max_y = acc_y;
      }

    if (all_accepted)
      printf ("sweep %d: the chain of the %d pairs is valid,"
              " travel-x %d  travel-y %d\n",
              sweep_index, SWIPE_FRAMES - 1,
              b_max_x - b_min_x, b_max_y - b_min_y);
    else
      printf ("sweep %d: the chain of the %d pairs is broken at pair %d (%s),"
              " the travel of the found offsets is travel-x %d  travel-y %d"
              " (diagnostic only)\n",
              sweep_index, SWIPE_FRAMES - 1, first_rejected,
              reasons[first_rejected],
              b_max_x - b_min_x, b_max_y - b_min_y);
  }

  /* The second phase: the frames are split into the chains of consecutive
   * accepted pairs.  EVERY stretch of at least eight frames inside such a
   * chain is a candidate, not only the maximal stretch of the chain: a
   * chain that drifts far grows a canvas that no longer fits into
   * 640x1024, and then the maximal stretch alone would hide the shorter
   * stretches of the same swipe that do fit.  A candidate is only used
   * when it is usable, that is when its canvas fits and when its y travel
   * is large enough; the candidate with the largest y travel wins and a
   * tie is won by the candidate with more frames.  A rejected pair ends a
   * chain, so two stretches are never joined across a rejected pair. */
  {
    int p = 0;

    while (p < SWIPE_FRAMES - 1)
      {
        int q;

        if (!accepted[p])
          {
            p++;
            continue;
          }

        /* The maximal chain of accepted pairs that starts at p. */
        q = p;
        while (q + 1 < SWIPE_FRAMES - 1 && accepted[q + 1])
          q++;

        /* Every contiguous stretch of that chain. */
        for (int first = p; first <= q; first++)
          for (int last = first; last <= q; last++)
            {
              /* The stretch spans the frames first .. last + 1. */
              if (last - first + 2 < MIN_RUN_FRAMES)
                continue;

              evaluate_run (offsets, first, last, origin_x, origin_y, &run);

              if (run.frames > longest_run)
                longest_run = run.frames;

              if (!have_candidate || run_is_better (&run, &best_candidate))
                {
                  best_candidate = run;
                  have_candidate = TRUE;
                }
              if (run.valid && (!have_valid || run_is_better (&run, &best_valid)))
                {
                  best_valid = run;
                  have_valid = TRUE;
                }
            }

        p = q + 1;
      }
  }

  if (!have_candidate)
    {
      snprintf (sweep->reason, sizeof (sweep->reason),
                "no stretch of %d frames with accepted pairs: the longest chain of"
                " accepted pairs spans %d frames and %d of the %d pairs were"
                " accepted",
                MIN_RUN_FRAMES, longest_run, accepted_pairs, SWIPE_FRAMES - 1);
      return FALSE;
    }

  if (!have_valid)
    {
      snprintf (sweep->reason, sizeof (sweep->reason),
                "the stretch of frames %d..%d (%d frames) with the largest y"
                " travel is not usable: %.150s",
                best_candidate.first_frame, best_candidate.last_frame,
                best_candidate.frames, best_candidate.reason);
      return FALSE;
    }

  /* The third phase: the layout of the frames of the selected stretch in
   * the canvas.  The origins are built inside the stretch, from the
   * offsets of its accepted pairs only. */
  evaluate_run (offsets, best_valid.first_pair, best_valid.last_pair,
                origin_x, origin_y, &run);

  sweep->first_frame = run.first_frame;
  sweep->last_frame = run.last_frame;
  sweep->run_min_ncc = run.min_ncc;
  sweep->travel_x = run.travel_x;
  sweep->travel_y = run.travel_y;

  /* The canvas is the full bounding UNION of the frames of the stretch: it
   * reaches from the smallest to the largest x origin and from the
   * smallest to the largest y origin, every frame adding its own 160x160
   * window to it.  No pixel that the finger really swept over is thrown
   * away. */
  canvas_x0 = run.min_x;
  canvas_x1 = run.max_x + SWIPE_FRAME_W;
  canvas_y0 = run.min_y;
  canvas_y1 = run.max_y + SWIPE_FRAME_H;
  canvas_w = canvas_x1 - canvas_x0;
  canvas_h = canvas_y1 - canvas_y0;

  pixels_n = (gsize) canvas_w * canvas_h;

  sum = g_try_malloc0 (pixels_n * sizeof (double));
  count = g_try_malloc0 (pixels_n * sizeof (guint32));
  pixels = g_try_malloc (pixels_n);
  mask = g_try_malloc (pixels_n);
  if (!sum || !count || !pixels || !mask)
    {
      g_free (sum);
      g_free (count);
      g_free (pixels);
      g_free (mask);
      snprintf (sweep->reason, sizeof (sweep->reason), "out of memory");
      return FALSE;
    }

  /* The mean of the raw pixels that cover a pixel of the output.  Only the
   * frames of the selected stretch are blended and the loop of every frame
   * is clamped to the intersection of that frame with the canvas, so that
   * only pixels that were really captured are ever blended. */
  for (int f = run.first_frame; f <= run.last_frame; f++)
    {
      const guint8 *frame = frames + f * SWIPE_FRAME_BYTES;
      int x_start = MAX (origin_x[f], canvas_x0);
      int x_end = MIN (origin_x[f] + SWIPE_FRAME_W, canvas_x1);
      int y_start = MAX (origin_y[f], canvas_y0);
      int y_end = MIN (origin_y[f] + SWIPE_FRAME_H, canvas_y1);

      for (int y = y_start; y < y_end; y++)
        {
          int local_y = y - origin_y[f];

          for (int x = x_start; x < x_end; x++)
            {
              gsize index = (gsize) (y - canvas_y0) * canvas_w + (x - canvas_x0);

              sum[index] += frame[local_y * SWIPE_FRAME_W + (x - origin_x[f])];
              count[index]++;
            }
        }
    }

  /* A pixel that no frame covers is unknown: it keeps the value of 255
   * and it is marked as unknown in the coverage mask.  No ridge is
   * invented there. */
  for (gsize i = 0; i < pixels_n; i++)
    {
      if (count[i] > 0)
        {
          pixels[i] = (guint8) (sum[i] / (double) count[i] + 0.5);
          mask[i] = 1;
          covered++;
        }
      else
        {
          pixels[i] = 255;
          mask[i] = 0;
        }
    }

  g_free (sum);
  g_free (count);

  sweep->pixels = pixels;
  sweep->mask = mask;
  sweep->covered = covered;
  sweep->width = canvas_w;
  sweep->height = canvas_h;
  sweep->ok = TRUE;

  printf ("sweep %d: the selected stretch is frames %d..%d (%d frames, %d accepted"
          " pairs)  min-ncc %.4f  travel-x %d  travel-y %d  canvas %dx%d"
          "  captured %u of %u pixels\n",
          sweep_index, run.first_frame, run.last_frame, run.frames, run.pairs,
          run.min_ncc, run.travel_x, run.travel_y, canvas_w, canvas_h,
          (guint) covered, (guint) pixels_n);

  return TRUE;
}

/* ------------------------------------------------------------------------- */

/* True when the square of the given radius around the native canvas
 * position lies inside the canvas and when every one of its pixels was
 * really captured.  A minutia that only touches the border of the
 * captured area would otherwise lend its features to the matcher even
 * though half of its neighbourhood is the padding of the canvas.
 *
 * The position is given in the NATIVE pixels of the canvas; a minutia of
 * a variant is mapped back with the scale of that variant. */
static gboolean
point_is_interior (const Sweep *sweep,
                   double       native_x,
                   double       native_y,
                   int          radius)
{
  int cx = (int) floor (native_x);
  int cy = (int) floor (native_y);
  int x0 = cx - radius;
  int x1 = cx + radius;
  int y0 = cy - radius;
  int y1 = cy + radius;

  /* The square has to lie inside the canvas. */
  if (x0 < 0 || y0 < 0 || x1 >= sweep->width || y1 >= sweep->height)
    return FALSE;

  /* And every one of its pixels has to be a pixel that was captured. */
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++)
      if (!sweep->mask[(gsize) y * sweep->width + x])
        return FALSE;

  return TRUE;
}

/* The contrast stretch of a canvas that holds unknown pixels.  Only the
 * pixels that were really captured take part in the stretch, so that the
 * padding of 255 does not take the contrast away from the real pixels of
 * the swipe.  The unknown pixels keep their value of 255. */
static guint8 *
contrast_stretch_masked (const guint8 *src,
                          const guint8 *mask,
                          gsize        len)
{
  guint8 *dst = g_memdup2 (src, len);
  guint8 lo = 255;
  guint8 hi = 0;
  gboolean have = FALSE;

  for (gsize i = 0; i < len; i++)
    {
      if (!mask[i])
        continue;

      if (!have)
        {
          lo = hi = src[i];
          have = TRUE;
          continue;
        }
      if (src[i] < lo)
        lo = src[i];
      if (src[i] > hi)
        hi = src[i];
    }

  if (!have || hi <= lo)
    return dst;

  for (gsize i = 0; i < len; i++)
    {
      if (!mask[i])
        {
          dst[i] = 255;
          continue;
        }
      dst[i] = (guint8) ((src[i] - lo) * 255 / (hi - lo));
    }

  return dst;
}

/* ------------------------------------------------------------------------- */

int
main (void)
{
  guint8 *input;
  Sweep sweeps[SWIPE_COUNT];
  guint counts[G_N_ELEMENTS (swipe_variants)][SWIPE_COUNT];
  guint interior_counts[G_N_ELEMENTS (swipe_variants)][SWIPE_COUNT];
  int self_scores[G_N_ELEMENTS (swipe_variants)][SWIPE_COUNT];
  int genuine[G_N_ELEMENTS (swipe_variants)];
  int failures = 0;
  gboolean have_both = FALSE;

  input = g_try_malloc (SWIPE_INPUT_BYTES);
  if (!input)
    {
      fprintf (stderr, "error: out of memory\n");
      return 1;
    }

  if (fread (input, 1, SWIPE_INPUT_BYTES, stdin) != SWIPE_INPUT_BYTES)
    {
      fprintf (stderr, "error: expected exactly %d bytes on stdin\n",
               SWIPE_INPUT_BYTES);
      g_free (input);
      return 1;
    }

  {
    guint8 extra;

    if (fread (&extra, 1, 1, stdin) != 0)
      {
        fprintf (stderr, "error: more than %d bytes on stdin\n",
                 SWIPE_INPUT_BYTES);
        g_free (input);
        return 1;
      }
  }

  for (int s = 0; s < SWIPE_COUNT; s++)
    assemble_sweep (input + (gsize) s * SWIPE_FRAMES * SWIPE_FRAME_BYTES,
                    s, &sweeps[s]);

  g_free (input);

  printf ("swipe assembly: %d sweeps of %d frames of %dx%d\n",
          SWIPE_COUNT, SWIPE_FRAMES, SWIPE_FRAME_W, SWIPE_FRAME_H);

  for (int s = 0; s < SWIPE_COUNT; s++)
    {
      const Sweep *sweep = &sweeps[s];

      if (sweep->ok)
        printf ("sweep %d: frames %d..%d  min-ncc %.4f  travel-x %d"
                "  travel-y %d  size %dx%d  captured %u of %u  OK\n",
                s, sweep->first_frame, sweep->last_frame, sweep->run_min_ncc,
                sweep->travel_x, sweep->travel_y,
                sweep->width, sweep->height,
                (guint) sweep->covered,
                (guint) ((gsize) sweep->width * sweep->height));
      else
        printf ("sweep %d: min-ncc %.4f  travel-x -  travel-y -"
                "  size -  REJECT (%s)\n",
                s, sweep->have_ncc ? sweep->min_ncc : 0.0, sweep->reason);
    }

  /* The tables are filled completely, so that no value is ever read that
   * was not measured.  A sweep that was rejected holds no image and is not
   * measured at all: its count of minutiae is 0 and its self score is -1,
   * which means that the score is unavailable. */
  for (size_t v = 0; v < G_N_ELEMENTS (swipe_variants); v++)
    {
      for (int s = 0; s < SWIPE_COUNT; s++)
        {
          counts[v][s] = 0;
          interior_counts[v][s] = 0;
          self_scores[v][s] = -1;
        }
      genuine[v] = -1;
    }

  /* Only when BOTH sweeps were rejected is there nothing to measure at
   * all.  One good sweep is still measured on its own, so that the
   * diagnosis of the good sweep does not need a new capture. */
  if (!sweeps[0].ok && !sweeps[1].ok)
    {
      printf ("genuine: unavailable, both sweeps were rejected\n");
      for (int s = 0; s < SWIPE_COUNT; s++)
        {
          g_free (sweeps[s].pixels);
          g_free (sweeps[s].mask);
        }
      return 0;
    }

  have_both = sweeps[0].ok && sweeps[1].ok;

  if (!have_both)
    {
      int rejected = sweeps[0].ok ? 1 : 0;

      printf ("genuine: unavailable, sweep %d was rejected\n", rejected);
    }

  /* The variants of the assembled images. */
  for (size_t v = 0; v < G_N_ELEMENTS (swipe_variants); v++)
    {
      const SwipeVariant *variant = &swipe_variants[v];
      struct xyt_struct xyt[SWIPE_COUNT];

      memset (xyt, 0, sizeof (xyt));

      for (int s = 0; s < SWIPE_COUNT; s++)
        {
          const Sweep *sweep = &sweeps[s];
          gsize stretched_len;
          guint8 *stretched;
          int dimension_x;
          int dimension_y;
          guint8 *data;
          FpImage *image;
          GError *error = NULL;
          guint count = 0;
          GPtrArray *minutiae;
          GPtrArray *interior;

          /* A rejected sweep holds no image, so there is nothing to detect
           * and nothing to convert.  It keeps the count of 0 and the self
           * score of -1, which means that both are unavailable. */
          if (!sweep->ok)
            continue;

          stretched_len = (gsize) sweep->width * sweep->height;
          stretched = contrast_stretch_masked (sweep->pixels, sweep->mask,
                                                stretched_len);
          dimension_x = (int) lround (sweep->width * variant->scale);
          dimension_y = (int) lround (sweep->height * variant->scale);

          if (dimension_x == sweep->width && dimension_y == sweep->height)
            data = g_memdup2 (stretched, stretched_len);
          else
            data = bilinear_resize (stretched, sweep->width, sweep->height,
                                    dimension_x, dimension_y);
          g_free (stretched);

          image = make_image (data, dimension_x, dimension_y,
                              PPMM_NATIVE * variant->scale, variant->inverted);
          g_free (data);

          if (detect_frame_minutiae (image, &count, &error) != 0)
            {
              if (is_no_minutiae_error (error))
                {
                  g_clear_error (&error);
                  count = 0;
                }
              else
                {
                  fflush (stdout);
                  fprintf (stderr, "error: %s: sweep %d: %s\n", variant->name, s,
                           error ? error->message : "minutiae detection failed");
                  g_clear_error (&error);
                  count = 0;
                  failures++;
                }
            }

          counts[v][s] = count;

          /* The minutiae that lie in the padding of the canvas or in the
           * border of the captured area must not lend their features to the
           * matcher.  They are collected in a temporary shallow array; the
           * array that libfprint owns is never changed and the minutiae
           * themselves are not touched at all. */
          minutiae = fp_image_get_minutiae (image);
          interior = g_ptr_array_new ();

          for (guint m = 0; minutiae && m < minutiae->len; m++)
            {
              struct fp_minutia *minutia = g_ptr_array_index (minutiae, m);
              double native_x = (double) minutia->x / variant->scale;
              double native_y = (double) minutia->y / variant->scale;

              if (point_is_interior (sweep, native_x, native_y,
                                     MINUTIA_RADIUS_NATIVE))
                g_ptr_array_add (interior, minutia);
            }

          interior_counts[v][s] = interior->len;

          /* Only the interior minutiae are converted and only they are used
           * for the self and for the genuine score. */
          minutiae_to_xyt (interior, dimension_x, dimension_y, &xyt[s]);

          g_ptr_array_free (interior, TRUE);
          g_object_unref (image);
        }

      /* The self score of every sweep and the genuine score between the
       * two sweeps, exactly as libfprint's fpi_print_match() computes
       * them.  The score of a sweep that was rejected does not exist, so
       * it stays -1, which means that it is unavailable. */
      for (int s = 0; s < SWIPE_COUNT; s++)
        {
          int probe_len;

          if (!sweeps[s].ok)
            continue;

          probe_len = bozorth_probe_init (&xyt[s]);

          self_scores[v][s] = bozorth_to_gallery (probe_len, &xyt[s], &xyt[s]);
        }

      /* The genuine score needs both sweeps; when one of them was
       * rejected there is no genuine score at all and -1 is reported,
       * never a score of 0 that could be read as a match. */
      if (have_both)
        {
          int probe_len = bozorth_probe_init (&xyt[0]);

          genuine[v] = bozorth_to_gallery (probe_len, &xyt[0], &xyt[1]);
        }
    }

  printf ("\n%-18s %-12s %-12s %-12s %s\n", "variant", "minutiae 0/1",
          "interior 0/1", "self 0/1", "genuine");
  for (size_t v = 0; v < G_N_ELEMENTS (swipe_variants); v++)
    {
      char genuine_text[16];

      /* The genuine score is only a number when both sweeps were
       * assembled; otherwise it is reported as unavailable and never as a
       * score of 0, which could be read as a match. */
      if (genuine[v] >= 0)
        snprintf (genuine_text, sizeof (genuine_text), "%4d", genuine[v]);
      else
        snprintf (genuine_text, sizeof (genuine_text), "unavailable");

      printf ("%-18s %5u/%-5u  %5u/%-5u  %4d/%-4d  %s\n",
              swipe_variants[v].name,
              counts[v][0], counts[v][1],
              interior_counts[v][0], interior_counts[v][1],
              self_scores[v][0], self_scores[v][1],
              genuine_text);
    }

  printf ("\nnote: only the genuine score can be compared with the threshold of 40;\n"
          "      the self score only measures how consistent one sweep is with itself.\n");
  printf ("note: 'minutiae' counts every detected minutia and 'interior' counts\n"
          "      the ones whose whole neighbourhood of %d native pixels was really\n"
          "      captured; only the interior ones are used for the scores.\n",
          MINUTIA_RADIUS_NATIVE);

  if (!have_both)
    {
      int rejected = sweeps[0].ok ? 1 : 0;

      printf ("note: sweep %d was rejected and was therefore not measured; its\n"
              "      count of minutiae is 0 and its self score is -1, both of which\n"
              "      mean that the value is unavailable and not that it is zero.\n"
              "      The genuine score needs both sweeps and is therefore\n"
              "      unavailable as well; it is NOT a score of 0 and NOT a match.\n",
              rejected);
    }

  for (int s = 0; s < SWIPE_COUNT; s++)
    {
      g_free (sweeps[s].pixels);
      g_free (sweeps[s].mask);
    }

  return failures ? 1 : 0;
}
