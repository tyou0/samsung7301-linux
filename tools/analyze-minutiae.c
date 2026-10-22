/*
 * analyze-minutiae.c - in-memory minutiae scale diagnostic for the Samsung
 * 04e8:7301 fingerprint sensor.
 *
 * Purpose: the sensor produces usable ridge images and enrolment completes,
 * but verification scores 0 at the default threshold.  The saved templates
 * contain 6..11 minutiae while NBIS needs at least
 * MIN_COMPUTABLE_BOZORTH_MINUTIAE (10) to compute a Bozorth matching.  The
 * libfprint driver enlarges the native 160x160 frame to 320x320 and NBIS uses
 * fixed pixel sized spatial parameters (the ppmm only feeds the reliability
 * computation), so the upscaling may well destroy the features.
 *
 * This tool therefore measures how many minutiae NBIS finds in the very same
 * frame at different scales and polarities.  It reads exactly 25600 raw
 * grayscale bytes (one 160x160 frame) from stdin, keeps every buffer in
 * memory and prints only the variant name and the minutiae count.  No image
 * and no template is ever written to disk.
 *
 * The variants are:
 *
 *   native 160x160, ppmm 19.69                (and colour inverted)
 *   bilinear 1.25x, 200x200, ppmm 24.61       (and colour inverted)
 *   bilinear 1.5x, 240x240, ppmm 29.54        (and colour inverted)
 *   bilinear 1.75x, 280x280, ppmm 34.46       (and colour inverted)
 *   bilinear 2x, 320x320, ppmm 39.38          (and colour inverted)
 *   bilinear 3x, 480x480, ppmm 59.07          (and colour inverted)
 *   bilinear 4x, 640x640, ppmm 78.76          (and colour inverted)
 *
 * The ppmm of a variant is 19.69 * dimension / 160.
 *
 * The original frame is contrast stretched once (min..max -> 0..255), exactly
 * like the driver does, before the variants are built.
 *
 * Only the public libfprint API is used: fp_image_new(),
 * fp_image_detect_minutiae(), fp_image_detect_minutiae_finish() and
 * fp_image_get_minutiae().  fpi-image.h is included for the FpImage fields
 * (data, ppmm, flags) that the public API does not expose; no private
 * function of libfprint is called.
 *
 * Build:
 *   cc -o analyze-minutiae analyze-minutiae.c \
 *      $(pkg-config --cflags --libs libfprint-2 gio-2.0) \
 *      -I/tmp/opencode/libfprint-upstream/libfprint -lm
 *
 * Usage:
 *   ./analyze-minutiae < frame.bin
 */

#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private struct layout, needed for image->data, image->ppmm and
 * image->flags.  It also pulls in the public fp-image.h. */
#include "fpi-image.h"

#define FRAME_WIDTH  160
#define FRAME_HEIGHT 160
#define FRAME_SIZE   (FRAME_WIDTH * FRAME_HEIGHT)

/* Upper bound for the main loop of a single variant. */
#define DETECT_TIMEOUT_S 10

/* ppmm of the native frame, as used by the driver. */
#define PPMM_NATIVE 19.69

typedef struct
{
  const char *name;
  int         dimension;   /* output width and height in pixels */
  gboolean    bilinear;    /* FALSE: the native frame is used unchanged */
  gboolean    inverted;    /* set FPI_IMAGE_COLORS_INVERTED */
} Variant;

/* The variants that are measured, in this order.  The ppmm of a variant is
 * PPMM_NATIVE * dimension / FRAME_WIDTH. */
static const Variant variants[] = {
  { "native-160-normal",           160, FALSE, FALSE },
  { "native-160-inverted",         160, FALSE, TRUE  },
  { "bilinear-1.25x-200-normal",   200, TRUE,  FALSE },
  { "bilinear-1.25x-200-inverted", 200, TRUE,  TRUE  },
  { "bilinear-1.5x-240-normal",    240, TRUE,  FALSE },
  { "bilinear-1.5x-240-inverted",  240, TRUE,  TRUE  },
  { "bilinear-1.75x-280-normal",   280, TRUE,  FALSE },
  { "bilinear-1.75x-280-inverted", 280, TRUE,  TRUE  },
  { "bilinear-2x-320-normal",      320, TRUE,  FALSE },
  { "bilinear-2x-320-inverted",    320, TRUE,  TRUE  },
  { "bilinear-3x-480-normal",      480, TRUE,  FALSE },
  { "bilinear-3x-480-inverted",    480, TRUE,  TRUE  },
  { "bilinear-4x-640-normal",      640, TRUE,  FALSE },
  { "bilinear-4x-640-inverted",    640, TRUE,  TRUE  },
};

typedef struct
{
  GMainLoop   *loop;
  GCancellable *cancellable;
  gboolean     timed_out;
  gboolean     failed;
  GError      *error;      /* owned */
  guint        count;
} DetectCtx;

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

/* Simple bilinear upscale, the libfprint internal fpi_image_resize() is not
 * exported by the shared library. */
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

/* ------------------------------------------------------------------------- */

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
detect_minutiae (FpImage  *image,
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

/* ------------------------------------------------------------------------- */

int
main (void)
{
  guint8 raw[FRAME_SIZE];
  guint8 *stretched;
  int failures = 0;
  size_t got;

  got = fread (raw, 1, FRAME_SIZE, stdin);
  if (got != FRAME_SIZE)
    {
      fprintf (stderr, "error: expected %d bytes on stdin, got %zu\n",
               FRAME_SIZE, got);
      return 1;
    }

  {
    guint8 extra;

    if (fread (&extra, 1, 1, stdin) != 0)
      {
        fprintf (stderr, "error: more than %d bytes on stdin\n", FRAME_SIZE);
        return 1;
      }
  }

  /* The raw input is kept as it is, the stretched copy feeds the variants. */
  stretched = contrast_stretch (raw, FRAME_SIZE);

  for (size_t i = 0; i < G_N_ELEMENTS (variants); i++)
    {
      const Variant *variant = &variants[i];
      int width = variant->dimension;
      int height = variant->dimension;
      gdouble ppmm = PPMM_NATIVE * variant->dimension / (gdouble) FRAME_WIDTH;
      guint8 *data;
      FpImage *image;
      guint count = 0;
      GError *error = NULL;

      if (variant->bilinear)
        data = bilinear_resize (stretched, FRAME_WIDTH, FRAME_HEIGHT, width, height);
      else
        data = g_memdup2 (stretched, FRAME_SIZE);

      image = make_image (data, width, height, ppmm, variant->inverted);
      g_free (data);

      if (detect_minutiae (image, &count, &error) != 0)
        {
          if (is_no_minutiae_error (error))
            {
              printf ("%-26s %u minutiae\n", variant->name, 0u);
              fflush (stdout);
              g_clear_error (&error);
              g_object_unref (image);
              continue;
            }

          fprintf (stderr, "error: %s: %s\n", variant->name,
                   error ? error->message : "minutiae detection failed");
          g_clear_error (&error);
          g_object_unref (image);
          failures++;
          continue;
        }

      g_object_unref (image);

      printf ("%-26s %u minutiae\n", variant->name, count);
      fflush (stdout);
    }

  g_free (stretched);

  return failures ? 1 : 0;
}
