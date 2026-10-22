/*
 * analyze-matching.c - Bozorth matching diagnostic for the Samsung 04e8:7301
 * fingerprint sensor.
 *
 * Purpose: the driver now produces enrolment samples with 10..13 minutiae,
 * which is the number the matcher needs, but the verification still scores
 * 0, 3, 7, 0 and 6 at the default threshold of 40.  Counting the features is
 * therefore not enough: the tool below measures the actual Bozorth score
 * between frames, the same way libfprint does it when it verifies a print.
 *
 * It reads exactly three raw 160x160 grayscale frames (76800 bytes) from
 * stdin, keeps every buffer in memory and prints for every variant the
 * number of minutiae of the three frames, the three self scores and the
 * three pairwise scores.  No image, no template and no coordinate is ever
 * written to disk or printed.
 *
 * The minutiae of a frame are detected with the public libfprint API
 * (fp_image_detect_minutiae()) and converted to the XYT representation with
 * the very same code as libfprint's fpi-print.c (lfs2nist_minutia_XYT(),
 * the theta correction and the sort_x_y order).  The scores come from the
 * NBIS matcher itself (bozorth_probe_init() and bozorth_to_gallery()), so
 * they are the numbers libfprint compares against the threshold of 40.  The
 * matcher minimum of 10 minutiae is not changed by this tool.
 *
 * The variants are:
 *
 *   native-160-normal, native-160-inverted
 *   bilinear-200-normal, bilinear-200-inverted
 *   bilinear-240-normal, bilinear-240-inverted
 *   bilinear-280-normal, bilinear-280-inverted
 *   bilinear-320-normal, bilinear-320-inverted
 *   pixman-2x-320-normal, pixman-2x-320-inverted
 *   localnorm-320-normal, localnorm-320-inverted
 *
 * "bilinear" is the manual bilinear interpolation of analyze-minutiae,
 * "pixman-2x" replicates fpi_image_resize() of the driver exactly (a8
 * pixels, a scale of two, a bilinear filter and no border), and
 * "localnorm" removes the local mean of a 21 native pixel window and scales
 * the local deviation by the inverse of the local standard deviation
 * (floored at 8), so that the result is 128 + 40 * z, before the frame is
 * enlarged.  The ppmm of a variant is 19.69 * dimension / 160.
 *
 * Every frame is contrast stretched on its own (min..max -> 0..255), exactly
 * like the driver does, before the variants are built.  The three frames
 * stay independent of each other.
 *
 * Build:
 *   cc -o /tmp/opencode/analyze-matching tools/analyze-matching.c \
 *      -I/tmp/opencode/libfprint-upstream/libfprint \
 *      -I/tmp/opencode/libfprint-upstream/libfprint/nbis/include \
 *      -I/tmp/opencode/libfprint-upstream/libfprint/nbis/libfprint-include \
 *      -I/tmp/opencode/libfprint-upstream/build \
 *      $(pkg-config --cflags --libs libfprint-2 gio-2.0 pixman-1) \
 *      /tmp/opencode/libfprint-upstream/build/libfprint/libnbis.a \
 *      -lm -lpthread
 *
 * Usage:
 *   ./analyze-matching < frames.bin
 */

#include <gio/gio.h>
#include <math.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

/* Private struct layout, needed for image->data, image->ppmm and
 * image->flags, and for the fp_minutia structure the NBIS headers use. */
#include "fpi-image.h"
#include "fpi-minutiae.h"

/* The NBIS headers of libfprint, exactly as the matcher uses them. */
#include <nbis.h>

#define FRAME_WIDTH  160
#define FRAME_HEIGHT 160
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT)
#define FRAME_COUNT  3
#define INPUT_SIZE   (FRAME_SIZE * FRAME_COUNT)

/* Upper bound for the main loop of a single detection. */
#define DETECT_TIMEOUT_S 10

/* ppmm of the native frame, as used by the driver. */
#define PPMM_NATIVE 19.69

/* Local normalisation: the window in native pixels, the floor of the local
 * standard deviation and the mean and the standard deviation of the result. */
#define LOCAL_WINDOW    21
#define LOCAL_STD_FLOOR 8.0
#define TARGET_MEAN     128.0
#define TARGET_STD      40.0

typedef enum {
  V_BILINEAR,   /* manual bilinear interpolation                      */
  V_PIXMAN,     /* the exact resize of the driver (fpi_image_resize)  */
  V_LOCALNORM,  /* local normalisation at native size, then bilinear  */
} VariantKind;

typedef struct
{
  const char *name;
  VariantKind kind;
  int         dimension;   /* output width and height in pixels */
  gboolean    inverted;    /* set FPI_IMAGE_COLORS_INVERTED     */
} Variant;

/* The variants that are measured, in this order.  The ppmm of a variant is
 * PPMM_NATIVE * dimension / FRAME_WIDTH. */
static const Variant variants[] = {
  { "native-160-normal",      V_BILINEAR,  160, FALSE },
  { "native-160-inverted",    V_BILINEAR,  160, TRUE  },
  { "bilinear-200-normal",    V_BILINEAR,  200, FALSE },
  { "bilinear-200-inverted",  V_BILINEAR,  200, TRUE  },
  { "bilinear-240-normal",    V_BILINEAR,  240, FALSE },
  { "bilinear-240-inverted",  V_BILINEAR,  240, TRUE  },
  { "bilinear-280-normal",    V_BILINEAR,  280, FALSE },
  { "bilinear-280-inverted",  V_BILINEAR,  280, TRUE  },
  { "bilinear-320-normal",    V_BILINEAR,  320, FALSE },
  { "bilinear-320-inverted",  V_BILINEAR,  320, TRUE  },
  { "pixman-2x-320-normal",   V_PIXMAN,    320, FALSE },
  { "pixman-2x-320-inverted", V_PIXMAN,    320, TRUE  },
  { "localnorm-320-normal",   V_LOCALNORM, 320, FALSE },
  { "localnorm-320-inverted", V_LOCALNORM, 320, TRUE  },
};

/* ------------------------------------------------------------------------- */

static guint8 *
contrast_stretch (const guint8 *src,
                  gsize        len)
{
  guint8 *dst = g_memdup2 (src, len);
  guint8 lo = 255;
  guint8 hi = 0;
  gsize i;

  if (!len)
    return dst;

  for (i = 0; i < len; i++)
    {
      if (src[i] < lo)
        lo = src[i];
      if (src[i] > hi)
        hi = src[i];
    }

  if (hi <= lo)
    return dst;

  for (i = 0; i < len; i++)
    dst[i] = (guint8) ((src[i] - lo) * 255 / (hi - lo));

  return dst;
}

/* Simple bilinear upscale, the same interpolation as analyze-minutiae uses. */
static guint8 *
bilinear_resize (const guint8 *src,
                 int           sw,
                 int           sh,
                 int           dw,
                 int           dh)
{
  guint8 *dst = g_malloc ((gsize) dw * dh);
  double scale_x = (double) sw / dw;
  double scale_y = (double) sh / dh;

  for (int y = 0; y < dh; y++)
    {
      double fy = (y + 0.5) * scale_y - 0.5;
      int y0 = (int) floor (fy);
      double wy = fy - y0;
      int y1 = y0 + 1;

      if (y0 < 0)
        {
          y0 = 0;
          wy = 0.0;
        }
      if (y0 > sh - 1)
        {
          y0 = sh - 1;
          wy = 0.0;
        }
      if (y1 < 0)
        y1 = 0;
      if (y1 > sh - 1)
        y1 = sh - 1;

      for (int x = 0; x < dw; x++)
        {
          double fx = (x + 0.5) * scale_x - 0.5;
          int x0 = (int) floor (fx);
          double wx = fx - x0;
          int x1 = x0 + 1;
          double p00, p01, p10, p11, top, bottom, value;

          if (x0 < 0)
            {
              x0 = 0;
              wx = 0.0;
            }
          if (x0 > sw - 1)
            {
              x0 = sw - 1;
              wx = 0.0;
            }
          if (x1 < 0)
            x1 = 0;
          if (x1 > sw - 1)
            x1 = sw - 1;

          p00 = src[y0 * sw + x0];
          p01 = src[y0 * sw + x1];
          p10 = src[y1 * sw + x0];
          p11 = src[y1 * sw + x1];

          top = p00 + (p01 - p00) * wx;
          bottom = p10 + (p11 - p10) * wx;
          value = top + (bottom - top) * wy;

          if (value < 0.0)
            value = 0.0;
          if (value > 255.0)
            value = 255.0;

          dst[y * dw + x] = (guint8) (value + 0.5);
        }
    }

  return dst;
}

/* The resize of the driver, replicated exactly: fpi_image_resize() scales
 * the a8 source by the factor with a bilinear filter and no border. */
static guint8 *
pixman_resize (const guint8 *src,
               int           sw,
               int           sh,
               int           factor)
{
  int dw = sw * factor;
  int dh = sh * factor;
  pixman_image_t *orig;
  pixman_image_t *resized;
  pixman_transform_t transform;
  guint8 *out;

  orig = pixman_image_create_bits (PIXMAN_a8, sw, sh, (uint32_t *) src, sw);
  resized = pixman_image_create_bits (PIXMAN_a8, dw, dh, NULL, dw);

  pixman_transform_init_identity (&transform);
  pixman_transform_scale (NULL, &transform,
                          pixman_int_to_fixed (factor),
                          pixman_int_to_fixed (factor));
  pixman_image_set_transform (orig, &transform);
  pixman_image_set_filter (orig, PIXMAN_FILTER_BILINEAR, NULL, 0);
  pixman_image_composite32 (PIXMAN_OP_SRC,
                            orig,      /* src  */
                            NULL,      /* mask */
                            resized,   /* dst  */
                            0, 0,      /* src x y   */
                            0, 0,      /* mask x y  */
                            0, 0,      /* dst x y   */
                            dw, dh);   /* width height */

  out = g_memdup2 (pixman_image_get_data (resized), (gsize) dw * dh);

  pixman_image_unref (orig);
  pixman_image_unref (resized);

  return out;
}

/* Local normalisation: the local mean of a 21 native pixel window is
 * removed, the local deviation is scaled by the inverse of the local
 * standard deviation (floored, so that noise is not amplified) and the
 * result is mapped to 128 + 40 * z. */
static guint8 *
local_normalize (const guint8 *src,
                 int           width,
                 int           height)
{
  int radius = LOCAL_WINDOW / 2;
  gsize stride = (gsize) width + 1;
  gsize integral_size = stride * ((gsize) height + 1);
  double *sum = g_malloc0 (integral_size * sizeof (double));
  double *sumsq = g_malloc0 (integral_size * sizeof (double));
  guint8 *dst = g_malloc ((gsize) width * height);

  /* Integral images of the values and of their squares. */
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        gsize here = (gsize) (y + 1) * stride + (x + 1);
        double value = src[y * width + x];

        sum[here] = value + sum[here - stride] + sum[here - 1] - sum[here - stride - 1];
        sumsq[here] = value * value + sumsq[here - stride] + sumsq[here - 1]
                    - sumsq[here - stride - 1];
      }

  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++)
      {
        int x0 = x - radius;
        int y0 = y - radius;
        int x1 = x + radius;
        int y1 = y + radius;
        double window_sum;
        double window_sumsq;
        double window_mean;
        double window_var;
        double window_std;
        double value;

        if (x0 < 0)
          x0 = 0;
        if (y0 < 0)
          y0 = 0;
        if (x1 > width - 1)
          x1 = width - 1;
        if (y1 > height - 1)
          y1 = height - 1;

        window_sum = sum[(gsize) (y1 + 1) * stride + (x1 + 1)]
                   - sum[(gsize) y0 * stride + (x1 + 1)]
                   - sum[(gsize) (y1 + 1) * stride + x0]
                   + sum[(gsize) y0 * stride + x0];
        window_sumsq = sumsq[(gsize) (y1 + 1) * stride + (x1 + 1)]
                     - sumsq[(gsize) y0 * stride + (x1 + 1)]
                     - sumsq[(gsize) (y1 + 1) * stride + x0]
                     + sumsq[(gsize) y0 * stride + x0];

        window_mean = window_sum / ((x1 - x0 + 1) * (y1 - y0 + 1));
        window_var = window_sumsq / ((x1 - x0 + 1) * (y1 - y0 + 1))
                   - window_mean * window_mean;
        if (window_var < 0.0)
          window_var = 0.0;
        window_std = sqrt (window_var);
        if (window_std < LOCAL_STD_FLOOR)
          window_std = LOCAL_STD_FLOOR;

        value = TARGET_MEAN
              + TARGET_STD * (src[y * width + x] - window_mean) / window_std;

        if (value < 0.0)
          value = 0.0;
        if (value > 255.0)
          value = 255.0;

        dst[(gsize) y * width + x] = (guint8) (value + 0.5);
      }

  g_free (sum);
  g_free (sumsq);

  return dst;
}

/* ------------------------------------------------------------------------- */

typedef struct
{
  GMainLoop   *loop;
  GCancellable *cancellable;
  gboolean     timed_out;
  gboolean     failed;
  GError      *error;      /* owned */
  guint        count;
} DetectCtx;

static void
detect_cb (GObject      *source,
           GAsyncResult *result,
           gpointer      user_data)
{
  DetectCtx *ctx = user_data;
  GError *error = NULL;

  if (!fp_image_detect_minutiae_finish (FP_IMAGE (source), result, &error))
    {
      ctx->failed = TRUE;
      ctx->error = error;
    }
  else
    {
      GPtrArray *minutiae = fp_image_get_minutiae (FP_IMAGE (source));

      ctx->count = minutiae ? minutiae->len : 0;
    }

  g_main_loop_quit (ctx->loop);
}

static gboolean
timeout_cb (gpointer user_data)
{
  DetectCtx *ctx = user_data;

  ctx->timed_out = TRUE;

  /* The detection is only asked to stop.  The loop keeps running until the
   * worker has finished and the callback has been delivered, so that
   * neither the context nor the image are touched while they are still in
   * use. */
  g_cancellable_cancel (ctx->cancellable);

  return G_SOURCE_REMOVE;
}

/* Run the minutiae detection of one image.  Returns 0 on success (the count
 * is stored in count), 1 on error (the error is stored in error). */
static int
detect_frame_minutiae (FpImage  *image,
                 guint    *count,
                 GError  **error)
{
  DetectCtx *ctx = g_new0 (DetectCtx, 1);
  GMainLoop *loop = g_main_loop_new (NULL, FALSE);
  GCancellable *cancellable = g_cancellable_new ();
  guint timeout_id;
  int ret = 1;

  ctx->loop = loop;
  ctx->cancellable = cancellable;

  timeout_id = g_timeout_add_seconds (DETECT_TIMEOUT_S, timeout_cb, ctx);

  fp_image_detect_minutiae (image, cancellable, detect_cb, ctx);
  g_main_loop_run (loop);

  if (ctx->timed_out)
    {
      /* The detection was cancelled and the worker has finished by now, so
       * the context and the image can be released safely. */
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
                   "minutiae detection timed out after %d seconds",
                   DETECT_TIMEOUT_S);
    }
  else if (ctx->failed)
    {
      g_propagate_error (error, ctx->error);
      ctx->error = NULL;
    }
  else
    {
      *count = ctx->count;
      ret = 0;
    }

  /* The timeout source removes itself when it fires, so it may only be
   * removed here when it is still active. */
  if (!ctx->timed_out)
    g_source_remove (timeout_id);

  g_clear_error (&ctx->error);
  g_free (ctx);

  g_object_unref (cancellable);
  g_main_loop_unref (loop);

  return ret;
}

/* libfprint reports "No minutiae found" as an error; for this diagnostic
 * that is a measurement of zero, not a failure of the tool. */
static gboolean
is_no_minutiae_error (const GError *error)
{
  return error != NULL &&
         g_error_matches (error, G_IO_ERROR, G_IO_ERROR_FAILED) &&
         strstr (error->message, "No minutiae") != NULL;
}

/* The conversion of libfprint's fpi-print.c, unchanged: the minutiae become
 * XYT points, the direction is corrected to (-180, 180] and the points are
 * sorted by their coordinates. */
static void
minutiae_to_xyt (GPtrArray       *minutiae,
                 int              width,
                 int              height,
                 struct xyt_struct *xyt)
{
  struct minutiae_struct c[MAX_FILE_MINUTIAE];
  struct fp_minutia **list;
  int nmin;
  int i;

  memset (xyt, 0, sizeof (*xyt));

  if (!minutiae)
    {
      xyt->nrows = 0;
      return;
    }

  nmin = (int) minutiae->len;
  if (nmin > MAX_BOZORTH_MINUTIAE)
    nmin = MAX_BOZORTH_MINUTIAE;

  list = g_new0 (struct fp_minutia *, nmin > 0 ? nmin : 1);
  for (i = 0; i < nmin; i++)
    list[i] = g_ptr_array_index (minutiae, i);

  for (i = 0; i < nmin; i++)
    {
      struct fp_minutia *minutia = list[i];

      lfs2nist_minutia_XYT (&c[i].col[0], &c[i].col[1], &c[i].col[2],
                            minutia, width, height);
      c[i].col[3] = sround (minutia->reliability * 100.0);

      if (c[i].col[2] > 180)
        c[i].col[2] -= 360;
    }

  qsort ((void *) &c, (size_t) nmin, sizeof (struct minutiae_struct), sort_x_y);

  for (i = 0; i < nmin; i++)
    {
      xyt->xcol[i]     = c[i].col[0];
      xyt->ycol[i]     = c[i].col[1];
      xyt->thetacol[i] = c[i].col[2];
    }
  xyt->nrows = nmin;

  g_free (list);
}

/* ------------------------------------------------------------------------- */

static FpImage *
make_image (const guint8 *data,
            int           width,
            int           height,
            gdouble       ppmm,
            gboolean      inverted)
{
  FpImage *image = fp_image_new (width, height);

  memcpy (image->data, data, (gsize) width * height);
  image->ppmm = ppmm;
  if (inverted)
    image->flags |= FPI_IMAGE_COLORS_INVERTED;

  return image;
}

/* Build the pixels of one frame of one variant. */
static guint8 *
variant_data (const Variant *variant,
              const guint8   *stretched,
              int           *dimension)
{
  int size = variant->dimension;

  switch (variant->kind)
    {
    case V_PIXMAN:
      *dimension = FRAME_WIDTH * 2;
      return pixman_resize (stretched, FRAME_WIDTH, FRAME_HEIGHT, 2);

    case V_LOCALNORM:
      {
        guint8 *normalized = local_normalize (stretched, FRAME_WIDTH, FRAME_HEIGHT);
        guint8 *scaled = bilinear_resize (normalized, FRAME_WIDTH, FRAME_HEIGHT,
                                         size, size);

        g_free (normalized);
        *dimension = size;
        return scaled;
      }

    case V_BILINEAR:
    default:
      *dimension = size;
      if (size == FRAME_WIDTH)
        return g_memdup2 (stretched, FRAME_SIZE);
      return bilinear_resize (stretched, FRAME_WIDTH, FRAME_HEIGHT, size, size);
    }
}

int
main (void)
{
  guint8 *raw;
  guint8 *stretched[FRAME_COUNT];
  int failures = 0;

  raw = g_malloc (INPUT_SIZE);
  if (fread (raw, 1, INPUT_SIZE, stdin) != INPUT_SIZE)
    {
      fprintf (stderr, "error: expected %d bytes on stdin\n", INPUT_SIZE);
      g_free (raw);
      return 1;
    }

  {
    guint8 extra;

    if (fread (&extra, 1, 1, stdin) != 0)
      {
        fprintf (stderr, "error: more than %d bytes on stdin\n", INPUT_SIZE);
        g_free (raw);
        return 1;
      }
  }

  /* Every frame is stretched on its own; the three frames stay independent
   * of each other. */
  for (int f = 0; f < FRAME_COUNT; f++)
    stretched[f] = contrast_stretch (raw + f * FRAME_SIZE, FRAME_SIZE);
  g_free (raw);

  printf ("%-24s %-22s %-22s %s\n", "variant", "minutiae 0/1/2",
          "self 0/1/2", "pairs 0-1 0-2 1-2");

  for (size_t v = 0; v < G_N_ELEMENTS (variants); v++)
    {
      const Variant *variant = &variants[v];
      struct xyt_struct xyt[FRAME_COUNT];
      guint counts[FRAME_COUNT];
      int self_scores[FRAME_COUNT];
      int pairs[3];
      gdouble ppmm;

      for (int f = 0; f < FRAME_COUNT; f++)
        {
          guint8 *data;
          int dimension = variant->dimension;
          FpImage *image;
          GError *error = NULL;
          guint count = 0;

          data = variant_data (variant, stretched[f], &dimension);
          ppmm = PPMM_NATIVE * dimension / (gdouble) FRAME_WIDTH;
          image = make_image (data, dimension, dimension, ppmm, variant->inverted);
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
                  fprintf (stderr, "error: %s: frame %d: %s\n", variant->name, f,
                           error ? error->message : "minutiae detection failed");
                  g_clear_error (&error);
                  count = 0;
                  failures++;
                }
            }

          counts[f] = count;
          minutiae_to_xyt (fp_image_get_minutiae (image), dimension, dimension,
                           &xyt[f]);
          g_object_unref (image);
        }

      /* The self scores and the pairwise scores, exactly as libfprint's
       * fpi_print_match() computes them. */
      for (int f = 0; f < FRAME_COUNT; f++)
        {
          int probe_len = bozorth_probe_init (&xyt[f]);

          self_scores[f] = bozorth_to_gallery (probe_len, &xyt[f], &xyt[f]);
        }

      {
        int index = 0;

        for (int f = 0; f < FRAME_COUNT; f++)
          for (int g = f + 1; g < FRAME_COUNT; g++)
            {
              int probe_len = bozorth_probe_init (&xyt[f]);

              pairs[index++] = bozorth_to_gallery (probe_len, &xyt[f], &xyt[g]);
            }
      }

      printf ("%-24s %3u/%3u/%3u          %3d/%3d/%3d          %3d %3d %3d\n",
              variant->name,
              counts[0], counts[1], counts[2],
              self_scores[0], self_scores[1], self_scores[2],
              pairs[0], pairs[1], pairs[2]);
      fflush (stdout);
    }

  for (int f = 0; f < FRAME_COUNT; f++)
    g_free (stretched[f]);

  return failures ? 1 : 0;
}
