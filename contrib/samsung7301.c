/*
 * Samsung 04e8:7301 fingerprint sensor driver for libfprint
 *
 * The sensor is an FPC (Fingerprint Cards AB) part that Samsung rebadges
 * under its own USB vendor ID.  It speaks the FPC "wbdi" transport, which is
 * shared with the 04e8:7305 and 04e8:730b readers.
 *
 * Wire format of one command exchange (all vendor control requests on EP0):
 *
 *   1. CTRL-OUT bRequest=0xc3 wValue=0x0000 wIndex=0x0000, 16 byte heartbeat
 *   2. CTRL-OUT bRequest=0xca wValue=0x0003 wIndex=<reply length>
 *   3. BULK-OUT the command bytes                       (EP 0x01)
 *   4. BULK-IN  <reply length> bytes                   (EP 0x82)
 *   5. CTRL-IN  bRequest=0xda wValue=0x0007, 2 bytes  (doorbell, optional)
 *
 * The session is opened with four CTRL-OUT 0xdb requests (wValue 0x0006 and
 * 0x000b, wIndex 0 and 1).
 *
 * The image read has to arm the bulk IN *before* the bulk OUT is written:
 * the sensor starts streaming the frame while the host is still writing the
 * padded command, so a sequential read only ever sees the tail of it.
 *
 * Matching
 * --------
 *
 * The frames of this sensor do not carry enough minutiae for libfprint's
 * NBIS/bozorth3 path, so the driver does not use the image device interface
 * any more.  It is a plain #FpDevice that captures the native 160x160 frames
 * and hands them to a fixed helper program, which runs the real SourceAFIS
 * matcher over them:
 *
 *   /usr/local/libexec/libfprint-samsung7301-afis
 *
 * The helper is started without a shell, with a fixed path and without any
 * argument, it reads the frames from stdin and prints one result line on
 * stdout.  The driver validates that line and the exit status of the helper
 * before it acts on it.  See contrib/sourceafis-matcher/README.md.
 *
 * The enrolled print is a #FPI_PRINT_RAW print whose data is the variant
 * "(saay)": the version string "samsung7301-sourceafis-v1" and the native
 * frames themselves.  A print of any other layout -- in particular an old
 * NBIS print -- is refused with an explicit re-enrolment error.
 *
 * Copyright (C) 2026 samsung7301-linux contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define FP_COMPONENT "samsung7301"

#include "drivers_api.h"

#include <math.h>

/* ── USB identifiers ─────────────────────────────────────────────────────── */

#define SAMSUNG7301_VID 0x04e8
#define SAMSUNG7301_PID 0x7301

static const FpIdEntry id_table[] = {
  { .vid = SAMSUNG7301_VID, .pid = SAMSUNG7301_PID },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

/* ── Endpoints ───────────────────────────────────────────────────────────── */

#define SAMSUNG7301_EP_OUT (0x01 | FPI_USB_ENDPOINT_OUT)
#define SAMSUNG7301_EP_IN  (0x82 | FPI_USB_ENDPOINT_IN)

/* ── Timeouts ────────────────────────────────────────────────────────────── */

#define CMD_TIMEOUT 2000
#define IMG_TIMEOUT 5000
#define POLL_DELAY  30

/* ── Image geometry ──────────────────────────────────────────────────────── */

/* The sensor reports a 160x160 frame; the Windows driver allocates a
 * 0x6402 byte staging buffer, which is exactly 160*160 + a two byte header. */
#define IMG_WIDTH  160
#define IMG_HEIGHT 160
#define IMG_HEADER 2
#define IMG_SIZE   (IMG_WIDTH * IMG_HEIGHT)
#define IMG_TRANSFER (IMG_SIZE + IMG_HEADER)

/* ── Capture ─────────────────────────────────────────────────────────────── */

/* One press of the finger is captured as a burst of real frames.
 *
 * The burst is the dominant cost of an attempt: the sensor produces one
 * frame about every 180 ms, so 32 frames take 5.8 s.  The matcher selects
 * at most four of them, and measured with the real stored frames the
 * best cross score is already the same for 12 probes as for 15, so a
 * longer burst only adds waiting time.  Sixteen frames keep a wide margin
 * over the four the matcher can use.
 */
#define CAPTURE_FRAMES 16

/* The delay between two frames of the burst. */
#define BURST_DELAY  80 /* ms */

/* The finger has to rest on the sensor before the frame is exposed. */
#define CONTACT_SETTLE 150 /* ms */
/* ── The matcher helper ──────────────────────────────────────────────────── */

/* The helper is a fixed, root owned program.  Its path is not taken from the
 * environment, from a print or from any other source. */
#define HELPER_PATH "/usr/local/libexec/libfprint-samsung7301-afis"

/* The magic of the wire protocol of the helper. */
#define HELPER_MAGIC "S731AF01"

/* The helper is bounded: it is killed when it takes too long. */
#define HELPER_TIMEOUT_MS 15000

/* The bounds of the wire protocol of the helper. */
#define HELPER_MAX_FRAMES 32
#define HELPER_MAX_SELECTED 4

/* The result of the helper is a single short line. */
#define HELPER_MAX_OUTPUT 512

/* The match threshold of the matcher.  It is the threshold the matcher
 * itself uses and it is not lowered anywhere. */
#define AFIS_THRESHOLD 40.0

/* ── The stored print ────────────────────────────────────────────────────── */

/* The version of the raw print layout. */
#define RAW_PRINT_VERSION "samsung7301-sourceafis-v1"

/* The layout of the raw print: the version and the native frames. */
#define RAW_PRINT_TYPE "(saay)"

/* The number of enrolment stages and the number of frames that are kept per
 * stage.  Five stages with at most four frames each stay well inside what
 * fprintd stores for a print. */
#define ENROLL_STAGES 5
#define MAX_STAGE_FRAMES 4
#define MAX_ENROLL_FRAMES (ENROLL_STAGES * MAX_STAGE_FRAMES)

/* ── Protocol constants ──────────────────────────────────────────────────── */

/* Vendor requests */
#define REQ_BULK      0xca  /* prime the reply length               */
#define REQ_HEARTBEAT 0xc3  /* frame heartbeat                     */
#define REQ_STATUS    0xda  /* doorbell / status read              */
#define REQ_SESSION   0xdb  /* session control                     */

#define FRAME_TYPE    0x0003 /* wValue of the 0xca request           */
#define DOORBELL_VAL  0x0007 /* wValue of the 0xda request           */

static const guint8 heartbeat_payload[16] = {
  0xc0, 0xc6, 0x2d, 0x00, 0x08, 0x00, 0xff, 0x01,
  0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Opcodes used by the driver. */
#define OP_STATUS      0x1c  /* read interrupt status with clear     */
#define OP_ARM         0x20  /* arm the sensor for a capture         */
#define OP_CAPGEO      0x54  /* capture geometry                     */
#define OP_IMAGESETUP  0x5c  /* image setup                          */
#define OP_IMAGERD     0x64  /* image read mode                      */
#define OP_ADCSETUP    0x9c  /* analogue front end setup             */
#define OP_ADCSHIFT    0xa0  /* ADC gain and shift                   */
#define OP_TRIGGER     0xc0  /* trigger the exposure                 */
#define OP_IMAGE_READ  0xc4  /* read the image                       */
#define OP_CAPREAD     0xd4  /* capacitive read                      */

/* Interrupt flags of the 0x1c reply (the FPC interrupt register).  The byte
 * seen on the wire is a bit field, so the flags have to be tested one by
 * one: 0x81 for instance is command done plus finger down. */
#define IRQ_FINGER_DOWN   0x01
#define IRQ_ERROR         0x04
#define IRQ_FIFO_NEW_DATA 0x20
#define IRQ_COMMAND_DONE  0x80
#define IRQ_REBOOT        0xff

/* Finger detection is decided by the capacitive zones: at least
 * DETECT_ZONES_ON of the twelve zones have to be covered to report a
 * finger, and at most DETECT_ZONES_OFF to report it as gone. */
#define DETECT_ZONES_ON  7
#define DETECT_ZONES_OFF 1

/* The hardware identification this profile was verified with. */
#define HWID_FAMILY   0x02
#define HWID_REVISION 0x1f

/* Index of the hardware id command in init_seq. */
#define INIT_HWID_INDEX 2

/* The sensor needs a moment to come out of the soft reset. */
#define RESET_DELAY 50 /* ms */

/* After the exposure is triggered the sensor needs a moment before the
 * frame is ready.  The interrupt register is polled until it reports the
 * new data; the poll is bounded so that a sensor that never finishes does
 * not block the capture state machine forever. */
#define EXPOSURE_POLL_DELAY 10  /* ms between two polls  */
#define EXPOSURE_POLL_MAX   500 /* polls, ~5 s in total */

/* The finger-off decision needs a fresh finger query, because the image
 * wait has already consumed the interrupt flags.  The query is repeated
 * until the sensor reports the command as done. */
#define FINGER_POLL_DELAY 5    /* ms between two polls  */
#define FINGER_POLL_MAX   200  /* polls, ~1 s in total */

/* ── Bring-up sequence ───────────────────────────────────────────────────── */

/* The sensor keeps the analogue front end it was shipped with, and the
 * driver must not overwrite the factory registers:
 *
 *   9c 55 40 00 3f 24                     adcsetup
 *   98 03 e9 bf 37 fe 05 19 32 9b 66      adcweighttbl
 *   a8 0f 1b                              pxlctrl
 *   8c 02                                 fngrdriveconf
 *   64 0e                                 imagerd
 *   5c 03                                 imagesetup
 *
 * With them the exposed frames show the ridges; the generic tables of the
 * Windows driver leave the sensor saturated.  Only the gain and the
 * detection threshold are written. */
static const guint8 init_reset[]   = { 0xf8 };
static const guint8 init_status[]  = { 0x1c, 0x00 };
static const guint8 init_hwid[]    = { 0xfc, 0x00, 0x00 };

/* Gain of the capacitive detection and its threshold.  The threshold is
 * the one that reads back as empty zones without a finger and as fully
 * covered zones with a finger. */
static const guint8 detect_gain[]   = { 0xa0, 0x06, 0x02 };
static const guint8 detect_thresh[] = { 0xd8, 0x60 };

/* Gain used while the frame is exposed. */
static const guint8 capture_gain[] = { 0xa0, 0x09, 0x02 };

/* Single byte commands used by the capture state machine.  These have to
 * have static storage: the transfers are asynchronous, so the buffers have
 * to stay valid until the transfer completes. */
static const guint8 cmd_arm[]     = { OP_ARM };
static const guint8 cmd_status[]  = { OP_STATUS, 0x00 };
static const guint8 cmd_trigger[] = { OP_TRIGGER };

/* Capture geometry, 160 x 160 pixels. */
static const guint8 cap_capgeo[]  = { 0x54, 0x00, 0xa0, 0x00, 0xa0 };

/* Capacitive read; the reply carries the twelve detection zones. */
static const guint8 cap_zones[]   = { 0xd4, 0x00, 0x00 };

typedef struct
{
  const guint8 *data;
  gsize         len;
  guint16       reply_len;
  gboolean      doorbell;
} SamsungCmd;

#define CMD(d)    { (d), sizeof (d), sizeof (d), FALSE }
#define CMD_DB(d) { (d), sizeof (d), sizeof (d), TRUE }

/* Bring-up: clear the interrupt status and read the chip identification. */
static const SamsungCmd init_seq[] = {
  CMD (init_status),
  CMD (init_status),
  CMD (init_hwid),
};

/* Detection profile: gain and threshold of the capacitive detection. */
static const SamsungCmd detect_seq[] = {
  CMD (detect_gain),
  CMD (detect_thresh),
};

/* Capture sequence: the capture gain and the geometry. */
static const SamsungCmd capture_seq[] = {
  CMD (capture_gain),
  CMD (cap_capgeo),
};

/* ── Frame helpers ───────────────────────────────────────────────────────── */

/* The capacitive read answers with the twelve detection zones as a bit map
 * in the last two bytes of the reply, big endian. */
static guint
s7301_decode_zones (const guint8 *resp)
{
  return ((resp[1] << 8) | resp[2]) & 0xfff;
}

/* Number of covered zones. */
static guint
s7301_count_zones (guint zones)
{
  guint count = 0;

  while (zones)
    {
      count += zones & 1;
      zones >>= 1;
    }

  return count;
}

/* ── The stored print ────────────────────────────────────────────────────── */

/* Build the data of a raw print: the version and the native frames. */
static GVariant *
s7301_build_raw_data (GPtrArray *frames)
{
  GVariantBuilder builder;
  GVariant *array;
  GVariant *result;
  guint i;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aay"));

  for (i = 0; i < frames->len; i++)
    g_variant_builder_add_value (&builder,
                                 g_variant_new_fixed_array (G_VARIANT_TYPE_BYTE,
                                                           g_ptr_array_index (frames, i),
                                                           IMG_SIZE, 1));

  array = g_variant_builder_end (&builder);

  /* The "@" takes the reference of the array over, it must not be released
   * here. */
  result = g_variant_new ("(s@aay)", RAW_PRINT_VERSION, array);

  return result;
}

/* Read the frames of a stored raw print.  Anything that is not a print of
 * this exact layout is refused, with an error that asks for a new
 * enrolment. */
static GPtrArray *
s7301_parse_raw_data (GVariant *data, GError **error)
{
  g_autoptr(GVariant) version_value = NULL;
  g_autoptr(GVariant) frames_value = NULL;
  g_autoptr(GPtrArray) frames = NULL;
  GVariantIter iter;
  GVariant *child = NULL;
  const gchar *version;
  gsize count;

  if (!data || !g_variant_is_of_type (data, G_VARIANT_TYPE (RAW_PRINT_TYPE)))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "the stored print is not a \"%s\" print, please re-enrol this finger",
                   RAW_PRINT_VERSION);
      return NULL;
    }

  version_value = g_variant_get_child_value (data, 0);
  version = g_variant_get_string (version_value, NULL);
  if (g_strcmp0 (version, RAW_PRINT_VERSION) != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "the stored print has the version \"%s\" instead of \"%s\", "
                   "please re-enrol this finger",
                   version ? version : "", RAW_PRINT_VERSION);
      return NULL;
    }

  frames_value = g_variant_get_child_value (data, 1);

  /* The number of the frames is checked before anything is copied. */
  count = g_variant_n_children (frames_value);
  if (count < 1 || count > MAX_ENROLL_FRAMES)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "the stored print holds %zu frames instead of 1..%d, "
                   "please re-enrol this finger",
                   count, MAX_ENROLL_FRAMES);
      return NULL;
    }

  frames = g_ptr_array_new_with_free_func (g_free);
  g_variant_iter_init (&iter, frames_value);

  while ((child = g_variant_iter_next_value (&iter)) != NULL)
    {
      gsize size = 0;
      const guint8 *bytes = g_variant_get_fixed_array (child, &size, 1);

      if (!bytes || size != IMG_SIZE)
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                       "the stored print holds a frame of %zu instead of %d bytes, "
                       "please re-enrol this finger",
                       size, IMG_SIZE);
          g_variant_unref (child);
          return NULL;
        }

      g_ptr_array_add (frames, g_memdup2 (bytes, size));
      g_variant_unref (child);
    }

  return g_steal_pointer (&frames);
}

/* ── Driver state ────────────────────────────────────────────────────────── */

typedef struct _FpiDeviceSamsung7301 FpiDeviceSamsung7301;

/* The result of one run of the matcher helper. */
typedef struct
{
  double quality_max;
  double best_cross;
  guint  selected_count;
  gint   indices[HELPER_MAX_SELECTED];
} S7301HelperResult;

struct _FpiDeviceSamsung7301
{
  FpDevice parent;

  FpiSsm *open_ssm;
  FpiSsm *ssm;

  guint   session_idx;
  guint   init_idx;
  guint   detect_idx;
  guint   seq_idx;
  guint   image_poll_count;
  guint   finger_poll_count;

  guint8 *img_buf;
  guint   image_pending;
  GError *image_error;

  /* The frames of the burst that is being captured. */
  GPtrArray *frames;

  /* The reference frames: the frames that were enrolled so far during an
   * enrolment, the stored print during a verification. */
  GPtrArray *refs;

  /* The matcher helper. */
  GSubprocess       *helper;
  GBytes            *helper_input;
  GBytes            *helper_stdout;
  GBytes            *helper_stderr;
  GSource           *helper_timeout;
  GError            *helper_error;
  gboolean           helper_done;
  gboolean           helper_timed_out;
  gint               helper_status;
  S7301HelperResult  helper_result;

  /* The operation. */
  gboolean enrolling;
  gboolean verifying;
  gboolean cancelled;
  guint    enroll_stage;
  FpPrint *enroll_print;
  FpiMatchResult verify_result;
  GError  *verify_error;

  gboolean finger_present;
  gboolean interface_claimed;
};

G_DECLARE_FINAL_TYPE (FpiDeviceSamsung7301, fpi_device_samsung7301, FPI,
                      DEVICE_SAMSUNG7301, FpDevice)

/* ── Command exchange ────────────────────────────────────────────────────── */

typedef void (*ExchangeDone) (FpDevice *dev, FpiSsm *ssm,
                              const guint8 *resp, gsize len, GError *error);

typedef struct
{
  const guint8 *cmd;
  gsize         cmd_len;
  guint16       prime_len;
  gboolean      doorbell;
  ExchangeDone  done_cb;
  guint8       *resp_buf;
  gsize         resp_len;
} ExchangeCtx;

static void s7301_ex_after_heartbeat (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_ex_after_prime    (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_ex_after_write    (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_ex_after_read     (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_ex_after_doorbell (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);

static void
s7301_ex_complete (FpDevice *dev, FpiSsm *ssm, ExchangeCtx *ctx, GError *error)
{
  ExchangeDone cb = ctx->done_cb;
  guint8 *buf = ctx->resp_buf;
  gsize n = ctx->resp_len;

  ctx->resp_buf = NULL;
  g_free (ctx);

  cb (dev, ssm, buf, n, error);
  g_free (buf);
}

static void
s7301_ex_after_heartbeat (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (err)
    {
      s7301_ex_complete (dev, t->ssm, ctx, err);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (n,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_BULK, FRAME_TYPE, ctx->prime_len, 0);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_ex_after_prime, ctx);
}

static void
s7301_ex_after_prime (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (err)
    {
      s7301_ex_complete (dev, t->ssm, ctx, err);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk_full (n, SAMSUNG7301_EP_OUT,
                                   (guint8 *) ctx->cmd, ctx->cmd_len, NULL);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_ex_after_write, ctx);
}

static void
s7301_ex_after_write (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (err)
    {
      s7301_ex_complete (dev, t->ssm, ctx, err);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (n, SAMSUNG7301_EP_IN, ctx->prime_len);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_ex_after_read, ctx);
}

static void
s7301_ex_after_read (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;
  FpiUsbTransfer *n;

  if (err)
    {
      s7301_ex_complete (dev, t->ssm, ctx, err);
      return;
    }

  ctx->resp_len = t->actual_length;
  ctx->resp_buf = g_memdup2 (t->buffer, t->actual_length);

  if (!ctx->doorbell)
    {
      s7301_ex_complete (dev, t->ssm, ctx, NULL);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (n,
                                 G_USB_DEVICE_DIRECTION_DEVICE_TO_HOST,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_STATUS, DOORBELL_VAL, 0x0000, 2);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_ex_after_doorbell, ctx);
}

static void
s7301_ex_after_doorbell (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  ExchangeCtx *ctx = ud;

  s7301_ex_complete (dev, t->ssm, ctx, err);
}

/* Run one command exchange. */
static void
s7301_exchange (FpDevice     *dev,
                 FpiSsm       *ssm,
                 const guint8 *cmd,
                 gsize         cmd_len,
                 guint16       prime_len,
                 gboolean      doorbell,
                 ExchangeDone  done_cb)
{
  ExchangeCtx *ctx = g_new0 (ExchangeCtx, 1);
  FpiUsbTransfer *t;

  ctx->cmd = cmd;
  ctx->cmd_len = cmd_len;
  ctx->prime_len = prime_len;
  ctx->doorbell = doorbell;
  ctx->done_cb = done_cb;

  t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_HEARTBEAT, 0x0000, 0x0000,
                                 sizeof (heartbeat_payload));
  memcpy (t->buffer, heartbeat_payload, sizeof (heartbeat_payload));
  fpi_usb_transfer_submit (t, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_ex_after_heartbeat, ctx);
}

/* ── Open ────────────────────────────────────────────────────────────────── */

enum {
  O_SESSION,
  O_RESET,
  O_INIT,
  O_DETECT,
  O_NUM_STATES,
};

static void s7301_run_session (FpDevice *dev, FpiSsm *ssm);
static void s7301_run_reset (FpDevice *dev, FpiSsm *ssm);
static void s7301_run_init (FpDevice *dev, FpiSsm *ssm);
static void s7301_run_detect (FpDevice *dev, FpiSsm *ssm);

static void
s7301_session_done (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (ud);

  if (error)
    {
      /* This part stalls the 0x000b re-init writes; only the 0x0006 pair is
       * mandatory, so tolerate a failure there. */
      if (self->session_idx <= 2)
        {
          fpi_ssm_mark_failed (t->ssm, error);
          return;
        }
      fp_dbg ("samsung7301: session re-init failed (ignored): %s", error->message);
      g_error_free (error);
    }
  s7301_run_session (dev, t->ssm);
}

static void
s7301_run_session (FpDevice *dev, FpiSsm *ssm)
{
  /* The session is opened with four 0xdb writes: wValue 0x0006 and 0x000b,
   * each with wIndex 0 and 1. */
  static const guint16 values[] = { 0x0006, 0x0006, 0x000b, 0x000b };
  static const guint16 indices[] = { 0x0000, 0x0001, 0x0000, 0x0001 };
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  FpiUsbTransfer *t;

  if (self->session_idx >= G_N_ELEMENTS (values))
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  t = fpi_usb_transfer_new (dev);
  t->ssm = ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_SESSION, values[self->session_idx],
                                 indices[self->session_idx], 0);
  self->session_idx++;
  fpi_usb_transfer_submit (t, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_session_done, self);
}

/* The hardware identification reply is the ack byte followed by the family
 * and the revision of the chip. */
static gboolean
s7301_hwid_is_supported (const guint8 *resp, gsize len)
{
  return len == 3 && resp[1] == HWID_FAMILY && resp[2] == HWID_REVISION;
}

static void
s7301_reset_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* The sensor needs a moment to come out of the reset. */
  fpi_ssm_next_state_delayed (ssm, RESET_DELAY);
}

static void
s7301_run_reset (FpDevice *dev, FpiSsm *ssm)
{
  s7301_exchange (dev, ssm, init_reset, sizeof (init_reset),
                  sizeof (init_reset), TRUE, s7301_reset_done);
}

static void
s7301_init_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* This profile was verified with one identification only.  Another one is
   * reported instead of falling back to a generic guess. */
  if (self->init_idx == INIT_HWID_INDEX + 1 && !s7301_hwid_is_supported (resp, len))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "unsupported hardware id %02x %02x, "
                                                     "this profile is verified for %02x %02x",
                                                     len > 1 ? resp[1] : 0,
                                                     len > 2 ? resp[2] : 0,
                                                     HWID_FAMILY, HWID_REVISION));
      return;
    }

  s7301_run_init (dev, ssm);
}

static void
s7301_run_init (FpDevice *dev, FpiSsm *ssm)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  const SamsungCmd *c;

  if (self->init_idx >= G_N_ELEMENTS (init_seq))
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  c = &init_seq[self->init_idx++];
  s7301_exchange (dev, ssm, c->data, c->len, c->reply_len, c->doorbell,
                  s7301_init_done);
}

static void
s7301_detect_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  s7301_run_detect (dev, ssm);
}

static void
s7301_run_detect (FpDevice *dev, FpiSsm *ssm)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  const SamsungCmd *c;

  if (self->detect_idx >= G_N_ELEMENTS (detect_seq))
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  c = &detect_seq[self->detect_idx++];
  s7301_exchange (dev, ssm, c->data, c->len, c->reply_len, c->doorbell,
                  s7301_detect_done);
}

static void
s7301_open_loop (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case O_SESSION:
      self->session_idx = 0;
      s7301_run_session (dev, ssm);
      break;

    case O_RESET:
      s7301_run_reset (dev, ssm);
      break;

    case O_INIT:
      self->init_idx = 0;
      s7301_run_init (dev, ssm);
      break;

    case O_DETECT:
      self->detect_idx = 0;
      s7301_run_detect (dev, ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
s7301_open_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  self->open_ssm = NULL;

  /* The interface stays claimed for the whole lifetime of the open device, so
   * a failed open has to give it back here.  The error of the open state
   * machine is the interesting one; a failure to release is only logged. */
  if (error && self->interface_claimed)
    {
      GError *release_error = NULL;

      self->interface_claimed = FALSE;
      if (!g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                           0, 0, &release_error))
        {
          fp_warn ("samsung7301: failed to release USB interface: %s",
                   release_error->message);
          g_clear_error (&release_error);
        }
    }

  fpi_device_open_complete (dev, error);
}

static void
s7301_dev_open (FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  GError *error = NULL;

  self->session_idx = 0;
  self->init_idx = 0;
  self->detect_idx = 0;

  /* Claim the interface before any transfer: the kernel refuses to hand the
   * endpoints to us unless we own interface 0, and fprintd's usbfs process
   * would otherwise grab it first. */
  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     0, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }

  self->interface_claimed = TRUE;

  self->open_ssm = fpi_ssm_new (dev, s7301_open_loop, O_NUM_STATES);
  fpi_ssm_start (self->open_ssm, s7301_open_complete);
}

static void
s7301_dev_close (FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  GError *error = NULL;

  g_clear_pointer (&self->img_buf, g_free);

  if (self->interface_claimed)
    {
      self->interface_claimed = FALSE;
      g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                      0, 0, &error);
    }

  fpi_device_close_complete (dev, error);
}

/* ── The matcher helper ──────────────────────────────────────────────────── */

/* Build the input of the helper: the magic, the two counts and the frames.
 * The probes are the frames of the burst, the references are the frames that
 * were enrolled so far. */
static GBytes *
s7301_helper_build_input (FpiDeviceSamsung7301 *self)
{
  GByteArray *buf = g_byte_array_new ();
  guint8 header[16];
  guint32 counts[2];
  guint i;

  memcpy (header, HELPER_MAGIC, 8);
  counts[0] = GUINT32_TO_LE ((guint32) self->frames->len);
  counts[1] = GUINT32_TO_LE ((guint32) self->refs->len);
  memcpy (header + 8, &counts[0], 4);
  memcpy (header + 12, &counts[1], 4);
  g_byte_array_append (buf, header, sizeof (header));

  for (i = 0; i < self->frames->len; i++)
    g_byte_array_append (buf, g_ptr_array_index (self->frames, i), IMG_SIZE);

  for (i = 0; i < self->refs->len; i++)
    g_byte_array_append (buf, g_ptr_array_index (self->refs, i), IMG_SIZE);

  return g_byte_array_free_to_bytes (buf);
}

/* Parse a number of the result line.  The whole token has to be a number
 * and the number has to be finite. */
static gboolean
s7301_parse_double (const gchar *text, double *value)
{
  gchar *end = NULL;
  gdouble parsed;

  if (!text || !*text)
    return FALSE;

  parsed = g_ascii_strtod (text, &end);
  if (end == text || *end != '\0')
    return FALSE;
  if (!isfinite (parsed))
    return FALSE;

  *value = parsed;
  return TRUE;
}

/* Parse an integer of the result line. */
static gboolean
s7301_parse_int (const gchar *text, gint64 *value)
{
  gchar *end = NULL;
  gint64 parsed;

  if (!text || !*text)
    return FALSE;

  parsed = g_ascii_strtoll (text, &end, 10);
  if (end == text || *end != '\0')
    return FALSE;

  *value = parsed;
  return TRUE;
}

/* Validate the result line of the helper.  The line has to be the one line
 * that the helper documents, every number has to be in range, and the
 * selected frames have to be distinct. */
static gboolean
s7301_helper_parse (FpiDeviceSamsung7301 *self,
                    GBytes *output,
                    S7301HelperResult *result,
                    GError **error)
{
  g_autofree gchar *text = NULL;
  g_auto(GStrv) tokens = NULL;
  gsize size = 0;
  const gchar *data;
  gint64 count;
  gint64 indices[HELPER_MAX_SELECTED];
  guint i;
  guint j;

  data = g_bytes_get_data (output, &size);

  /* The result of the helper is a single short line. */
  if (size == 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed nothing");
      return FALSE;
    }

  if (size > HELPER_MAX_OUTPUT)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed %zu bytes instead of at most %d",
                   size, HELPER_MAX_OUTPUT);
      return FALSE;
    }

  if (memchr (data, '\0', size) != NULL)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed a zero byte");
      return FALSE;
    }

  /* Exactly one line, and it has to be ended by the newline. */
  if (data[size - 1] != '\n')
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper did not end its line");
      return FALSE;
    }

  if (memchr (data, '\n', size - 1) != NULL)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed more than one line");
      return FALSE;
    }

  text = g_strndup (data, size - 1);

  tokens = g_strsplit_set (text, " \t", -1);
  if (g_strv_length (tokens) != 8)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed %u fields instead of 8",
                   g_strv_length (tokens));
      return FALSE;
    }

  for (i = 0; i < 8; i++)
    {
      if (!tokens[i][0])
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "the matcher helper printed an empty field %u", i);
          return FALSE;
        }
    }

  if (g_strcmp0 (tokens[0], "OK") != 0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper did not report a result");
      return FALSE;
    }

  if (!s7301_parse_double (tokens[1], &result->quality_max) ||
      !s7301_parse_double (tokens[2], &result->best_cross))
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed a malformed score");
      return FALSE;
    }

  /* The scores of the matcher are not bounded from above. */
  if (result->quality_max < 0.0 || result->best_cross < 0.0)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed a negative score");
      return FALSE;
    }

  if (!s7301_parse_int (tokens[3], &count) ||
      count < 0 || count > HELPER_MAX_SELECTED)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper printed a malformed frame count");
      return FALSE;
    }
  result->selected_count = (guint) count;

  for (i = 0; i < HELPER_MAX_SELECTED; i++)
    {
      if (!s7301_parse_int (tokens[4 + i], &indices[i]))
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "the matcher helper printed a malformed frame index");
          return FALSE;
        }
    }

  /* The indices are checked before they are cast, so that a huge number
   * cannot wrap into a valid one. */
  for (i = 0; i < result->selected_count; i++)
    {
      if (indices[i] < 0 || indices[i] >= (gint64) self->frames->len)
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "the matcher helper selected the frame %" G_GINT64_FORMAT
                       " out of %u",
                       indices[i], self->frames->len);
          return FALSE;
        }

      result->indices[i] = (gint) indices[i];
    }

  /* The unused slots have to be marked as such. */
  for (i = result->selected_count; i < HELPER_MAX_SELECTED; i++)
    {
      if (indices[i] != -1)
        {
          g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                       "the matcher helper printed an unused frame index %"
                       G_GINT64_FORMAT,
                       indices[i]);
          return FALSE;
        }

      result->indices[i] = -1;
    }

  /* The selected frames have to exist and they have to be distinct. */
  for (i = 0; i < result->selected_count; i++)
    {
      for (j = 0; j < i; j++)
        {
          if (result->indices[j] == result->indices[i])
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "the matcher helper selected the frame %d twice",
                           result->indices[i]);
              return FALSE;
            }
        }

      /* The selected frames have to be distinct frames as well. */
      for (j = 0; j < i; j++)
        {
          if (memcmp (g_ptr_array_index (self->frames, result->indices[i]),
                      g_ptr_array_index (self->frames, result->indices[j]), IMG_SIZE) == 0)
            {
              g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                           "the matcher helper selected the same frame twice");
              return FALSE;
            }
        }
    }

  /* A selected frame is eligible, so the capture was strong enough. */
  if (result->selected_count > 0 && result->quality_max < AFIS_THRESHOLD)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper selected a frame of the quality %g",
                   result->quality_max);
      return FALSE;
    }

  /* With references the selected frames have to match them. */
  if (result->selected_count > 0 && self->refs->len > 0 &&
      result->best_cross < AFIS_THRESHOLD)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                   "the matcher helper selected a frame that only scored %g",
                   result->best_cross);
      return FALSE;
    }

  return TRUE;
}

static void s7301_helper_maybe_finish (FpiDeviceSamsung7301 *self);

static void
s7301_helper_timeout_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (user_data);

  self->helper_timeout = NULL;
  self->helper_timed_out = TRUE;

  /* The helper is killed; the completion of the subprocess decides how the
   * operation ends, so that nothing is reported while it is still running. */
  if (self->helper && !self->helper_done)
    g_subprocess_force_exit (self->helper);
}

/* The child is reaped.  Only now it is really gone, so only now the run of
 * the helper is over. */
static void
s7301_helper_reap_cb (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (user_data);
  GError *error = NULL;

  g_subprocess_wait_finish (G_SUBPROCESS (source), res, &error);
  g_clear_error (&error);

  /* The child is gone.  The run of the helper failed, whatever the reason
   * of the failed communication was. */
  self->helper_done = TRUE;
  self->helper_status = -1;

  s7301_helper_maybe_finish (self);
}

/* The helper is done.  This is the only place where the operation may
 * continue. */
static void
s7301_helper_communicate_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (user_data);
  g_autoptr(GBytes) stdout_bytes = NULL;
  g_autoptr(GBytes) stderr_bytes = NULL;
  g_autoptr(GError) error = NULL;

  if (!g_subprocess_communicate_finish (G_SUBPROCESS (source), res,
                                        &stdout_bytes, &stderr_bytes, &error))
    {
      /* A failed communication does not mean that the child is gone, so it
       * is killed and reaped first.  Nothing at all is reported before that,
       * and the state machine stays alive until the child is reaped. */
      self->helper_error = g_steal_pointer (&error);

      if (!self->cancelled)
        fp_dbg ("samsung7301: the matcher helper failed: %s",
                self->helper_error->message);

      g_subprocess_force_exit (self->helper);
      g_subprocess_wait_async (self->helper, NULL, s7301_helper_reap_cb, self);
      return;
    }

  /* The communication succeeded, so the child has exited. */
  self->helper_done = TRUE;
  self->helper_status = g_subprocess_get_if_exited (G_SUBPROCESS (source))
                          ? g_subprocess_get_exit_status (G_SUBPROCESS (source))
                          : -1;

  /* The output is kept until the result was validated. */
  self->helper_stdout = g_steal_pointer (&stdout_bytes);
  self->helper_stderr = g_steal_pointer (&stderr_bytes);

  s7301_helper_maybe_finish (self);
}

/* The resources of one run of the helper.  They are released as soon as the
 * result was validated, so that a retry does not pile up the raw buffers of
 * the previous runs. */
static void
s7301_helper_cleanup (FpiDeviceSamsung7301 *self)
{
  g_clear_pointer (&self->helper, g_object_unref);
  g_clear_pointer (&self->helper_input, g_bytes_unref);
  g_clear_pointer (&self->helper_stdout, g_bytes_unref);
  g_clear_pointer (&self->helper_stderr, g_bytes_unref);
  g_clear_pointer (&self->helper_error, g_error_free);
}

/* The helper is done when it is really gone.  Only then the operation may
 * continue. */
static void
s7301_helper_maybe_finish (FpiDeviceSamsung7301 *self)
{
  GError *error = NULL;

  if (!self->helper_done)
    return;

  if (!self->ssm)
    return;

  /* The timeout is not needed any more. */
  if (self->helper_timeout)
    {
      g_clear_pointer (&self->helper_timeout, g_source_destroy);
    }

  if (self->cancelled || fpi_device_action_is_cancelled (FP_DEVICE (self)))
    {
      fpi_ssm_mark_failed (self->ssm,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "the operation was cancelled"));
      return;
    }

  if (self->helper_timed_out)
    {
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "the matcher helper did not finish "
                                                     "within %d ms",
                                                     HELPER_TIMEOUT_MS));
      return;
    }

  if (self->helper_status != 0)
    {
      GError *failure = NULL;

      if (self->helper_error)
        failure = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "the matcher helper failed with the "
                                             "status %d: %s",
                                             self->helper_status,
                                             self->helper_error->message);
      else
        failure = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                             "the matcher helper failed with the "
                                             "status %d",
                                             self->helper_status);

      fpi_ssm_mark_failed (self->ssm, failure);
      return;
    }

  /* The helper does not write to stderr when it succeeds. */
  if (self->helper_stderr && g_bytes_get_size (self->helper_stderr) > 0)
    {
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "the matcher helper wrote to stderr"));
      return;
    }

  if (!self->helper_stdout)
    {
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "the matcher helper printed nothing"));
      return;
    }

  if (!s7301_helper_parse (self, self->helper_stdout, &self->helper_result, &error))
    {
      fpi_ssm_mark_failed (self->ssm, error);
      return;
    }

  /* The run is over, its buffers are not needed any more.  The failure
   * paths release them in the completion of the state machine. */
  s7301_helper_cleanup (self);

  fpi_ssm_next_state (self->ssm);
}

/* Start the helper and hand it the frames. */
static void
s7301_helper_start (FpiDeviceSamsung7301 *self)
{
  g_autoptr(GError) error = NULL;
  const gchar *argv[] = { HELPER_PATH, NULL };

  /* A previous run of the helper must not be left behind.  It is only ever
   * released here when it is really done, never while it is running. */
  if (self->helper_done)
    s7301_helper_cleanup (self);

  self->helper_timed_out = FALSE;
  self->helper_done = FALSE;
  self->helper_status = -1;
  self->helper_stdout = NULL;
  self->helper_stderr = NULL;

  /* The counts have to be inside the bounds of the protocol of the helper. */
  if (self->frames->len < 1 || self->frames->len > HELPER_MAX_FRAMES ||
      self->refs->len > HELPER_MAX_FRAMES)
    {
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "%u frames and %u references are "
                                                     "outside the bounds of the matcher",
                                                     self->frames->len,
                                                     self->refs->len));
      return;
    }

  /* No shell, no argument, no path from the environment. */
  self->helper = g_subprocess_newv (argv,
                                    G_SUBPROCESS_FLAGS_STDIN_PIPE |
                                    G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                    &error);
  if (!self->helper)
    {
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                     "the matcher helper could not be "
                                                     "started: %s",
                                                     error->message));
      return;
    }

  /* The input is kept until the helper is done. */
  self->helper_input = s7301_helper_build_input (self);

  self->helper_timeout = fpi_device_add_timeout (FP_DEVICE (self),
                                                  HELPER_TIMEOUT_MS,
                                                  s7301_helper_timeout_cb,
                                                  self, NULL);

  g_subprocess_communicate_async (self->helper, self->helper_input, NULL,
                                  s7301_helper_communicate_done, self);
}

/* ── Capture ─────────────────────────────────────────────────────────────── */

enum {
  C_DETECT_SETUP,
  C_ARM,
  C_STATUS,
  C_ZONES,
  C_SETTLE,
  C_SETUP,
  C_TRIGGER,
  C_WAIT_IMAGE,
  C_READ,
  C_RESTORE_DETECT,
  C_WAIT_OFF,
  C_WAIT_OFF_STATUS,
  C_WAIT_OFF_ZONES,
  C_HELPER,
  C_RESULT,
  C_NUM_STATES,
};

static void s7301_run_capture_seq (FpiDeviceSamsung7301 *self);
static void s7301_read_image (FpiDeviceSamsung7301 *self);
static void s7301_burst_next (FpiDeviceSamsung7301 *self);
static void s7301_result (FpiDeviceSamsung7301 *self);

static void
s7301_arm_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->finger_poll_count = 0;
  fpi_ssm_next_state_delayed (ssm, FINGER_POLL_DELAY);
}

/* Wait for the arm command to be finished before the zones are read. */
static void
s7301_status_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  guint8 irq;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (len != 2)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short interrupt reply (%zu bytes)",
                                                     len));
      return;
    }

  irq = resp[1];
  fp_dbg ("samsung7301: interrupt %02x", irq);

  if (irq == IRQ_REBOOT || (irq & IRQ_ERROR))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "sensor reported interrupt %02x",
                                                     irq));
      return;
    }

  if (!(irq & IRQ_COMMAND_DONE))
    {
      if (++self->finger_poll_count >= FINGER_POLL_MAX)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "arm not finished after %u polls "
                                                         "(last interrupt %02x)",
                                                         self->finger_poll_count,
                                                         irq));
          return;
        }

      fpi_ssm_jump_to_state_delayed (ssm, C_STATUS, FINGER_POLL_DELAY);
      return;
    }

  fpi_ssm_next_state (ssm);
}

/* The capacitive zones decide whether a finger is on the sensor; the
 * interrupt bits alone do not tell. */
static void
s7301_zones_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  guint zones;
  guint count;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (len != 3)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short zone reply (%zu bytes)",
                                                     len));
      return;
    }

  zones = s7301_decode_zones (resp);
  count = s7301_count_zones (zones);
  fp_dbg ("samsung7301: zones %03x (%u covered)", zones, count);

  if (count < DETECT_ZONES_ON)
    {
      fpi_ssm_jump_to_state_delayed (ssm, C_ARM, POLL_DELAY);
      return;
    }

  if (!self->finger_present)
    {
      self->finger_present = TRUE;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_PRESENT);
    }

  /* The finger is only on the sensor, it has to settle before the frame is
   * exposed. */
  fpi_ssm_next_state_delayed (ssm, CONTACT_SETTLE);
}

static void
s7301_setup_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->seq_idx++;
  if (self->seq_idx < G_N_ELEMENTS (capture_seq))
    s7301_run_capture_seq (self);
  else
    fpi_ssm_next_state (ssm);
}

static void
s7301_run_capture_seq (FpiDeviceSamsung7301 *self)
{
  const SamsungCmd *c = &capture_seq[self->seq_idx];

  s7301_exchange (FP_DEVICE (self), self->ssm, c->data, c->len,
                  c->reply_len, c->doorbell, s7301_setup_done);
}

static void
s7301_trigger_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* The exposure has only been started; the frame is read once the sensor
   * says that it is ready. */
  fpi_ssm_next_state (ssm);
}

/* Wait for the sensor to report that the exposed frame is ready.  The
 * FPC reference driver does the same: the capture command is followed by a
 * wait for the FIFO-new-data interrupt before the image is fetched. */
static void
s7301_wait_image_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  guint8 irq;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (len != 2)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short interrupt reply (%zu bytes)",
                                                     len));
      return;
    }

  irq = resp[1];
  fp_dbg ("samsung7301: interrupt %02x", irq);

  /* An error or a reboot is reported before the readiness flag: a reboot
   * has all the bits set and would otherwise look like new data. */
  if (irq == IRQ_REBOOT || (irq & IRQ_ERROR))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "sensor reported interrupt %02x",
                                                     irq));
      return;
    }

  if (irq & IRQ_FIFO_NEW_DATA)
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  if (++self->image_poll_count >= EXPOSURE_POLL_MAX)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "no frame after %u polls "
                                                     "(last interrupt %02x)",
                                                     self->image_poll_count,
                                                     irq));
      return;
    }

  fpi_ssm_jump_to_state_delayed (ssm, C_WAIT_IMAGE, EXPOSURE_POLL_DELAY);
}

/* The image read needs the bulk IN to be armed before the bulk OUT is
 * written, so it does not use the sequential exchange above. */
static void s7301_img_after_read (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_img_after_write (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_img_after_heartbeat (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_img_after_prime (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);

static void
s7301_read_image (FpiDeviceSamsung7301 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpiUsbTransfer *t;

  g_clear_pointer (&self->img_buf, g_free);
  self->img_buf = g_malloc0 (IMG_TRANSFER);
  self->img_buf[0] = OP_IMAGE_READ;
  g_clear_error (&self->image_error);

  t = fpi_usb_transfer_new (dev);
  t->ssm = self->ssm;
  t->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (t,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_HEARTBEAT, 0x0000, 0x0000,
                                 sizeof (heartbeat_payload));
  memcpy (t->buffer, heartbeat_payload, sizeof (heartbeat_payload));
  fpi_usb_transfer_submit (t, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_img_after_heartbeat, self);
}

static void
s7301_img_after_heartbeat (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (ud);
  FpiUsbTransfer *n;

  if (err)
    {
      fpi_ssm_mark_failed (self->ssm, err);
      return;
    }

  n = fpi_usb_transfer_new (dev);
  n->ssm = t->ssm;
  n->short_is_error = FALSE;
  fpi_usb_transfer_fill_control (n,
                                 G_USB_DEVICE_DIRECTION_HOST_TO_DEVICE,
                                 G_USB_DEVICE_REQUEST_TYPE_VENDOR,
                                 G_USB_DEVICE_RECIPIENT_DEVICE,
                                 REQ_BULK, FRAME_TYPE, IMG_TRANSFER, 0);
  fpi_usb_transfer_submit (n, CMD_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           s7301_img_after_prime, self);
}

static void
s7301_img_after_prime (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (ud);
  FpDevice *pdev = FP_DEVICE (self);
  FpiUsbTransfer *rd, *wr;

  if (err)
    {
      fpi_ssm_mark_failed (self->ssm, err);
      return;
    }

  self->image_pending = 2;

  /* Arm the read before the write: the sensor starts streaming the frame
   * while the host is still writing the padded command. */
  rd = fpi_usb_transfer_new (pdev);
  rd->ssm = t->ssm;
  rd->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (rd, SAMSUNG7301_EP_IN, IMG_TRANSFER);
  fpi_usb_transfer_submit (rd, IMG_TIMEOUT,
                           fpi_device_get_cancellable (pdev),
                           s7301_img_after_read, self);

  wr = fpi_usb_transfer_new (pdev);
  wr->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (wr, SAMSUNG7301_EP_OUT, IMG_TRANSFER);
  memcpy (wr->buffer, self->img_buf, IMG_TRANSFER);
  fpi_usb_transfer_submit (wr, IMG_TIMEOUT,
                           fpi_device_get_cancellable (pdev),
                           s7301_img_after_write, self);
}

/* One frame of the burst was read.  Every frame that was read is a real
 * frame of the sensor, so all of them are kept; the matcher decides which
 * of them carry any ridge information. */
static void
s7301_burst_next (FpiDeviceSamsung7301 *self)
{
  if (self->cancelled || fpi_device_action_is_cancelled (FP_DEVICE (self)))
    {
      fpi_ssm_mark_failed (self->ssm,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "the operation was cancelled"));
      return;
    }

  if (self->frames->len < CAPTURE_FRAMES)
    {
      /* The next frame of the burst; the capture gain stays in place. */
      fpi_ssm_jump_to_state_delayed (self->ssm, C_TRIGGER, BURST_DELAY);
      return;
    }

  /* The burst is complete. */
  fpi_ssm_next_state (self->ssm);
}

/* Both transfers must finish before the state machine can advance or be
 * freed. */
static void
s7301_img_transfer_done (FpiDeviceSamsung7301 *self, GError *err)
{
  if (err)
    {
      if (!self->image_error)
        self->image_error = err;
      else
        g_error_free (err);
    }

  if (--self->image_pending)
    return;

  if (self->image_error)
    {
      fpi_ssm_mark_failed (self->ssm, g_steal_pointer (&self->image_error));
      return;
    }

  g_ptr_array_add (self->frames, g_memdup2 (self->img_buf + IMG_HEADER, IMG_SIZE));
  fp_dbg ("samsung7301: captured frame %u of %u",
          self->frames->len, CAPTURE_FRAMES);

  s7301_burst_next (self);
}

static void
s7301_img_after_write (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (ud);

  s7301_img_transfer_done (self, err);
}

static void
s7301_img_after_read (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (ud);

  if (!err && t->actual_length != IMG_TRANSFER)
    err = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                    "short image transfer (%zu bytes)",
                                    t->actual_length);
  if (!err)
    memcpy (self->img_buf, t->buffer, IMG_TRANSFER);

  s7301_img_transfer_done (self, err);
}

/* The detection profile is written again on every activation, because the
 * capture gain can still be in place after a cancelled scan. */
static void
s7301_detect_setup_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fpi_ssm_next_state (ssm);
}

/* The detection gain is restored after the exposure. */
static void
s7301_restore_detect_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fpi_ssm_next_state (ssm);
}

/* A fresh finger query has to be issued before the finger-off decision can
 * be made: the interrupt register alone does not tell, because the image
 * wait has already consumed the flags. */
static void
s7301_wait_off_arm_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  self->finger_poll_count = 0;
  fpi_ssm_next_state_delayed (ssm, FINGER_POLL_DELAY);
}

static void
s7301_wait_off_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  guint8 irq;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (len != 2)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short interrupt reply (%zu bytes)",
                                                     len));
      return;
    }

  irq = resp[1];
  fp_dbg ("samsung7301: interrupt %02x", irq);

  if (irq == IRQ_REBOOT || (irq & IRQ_ERROR))
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "sensor reported interrupt %02x",
                                                     irq));
      return;
    }

  /* The query is only answered when the sensor reports the command as
   * done; until then the interrupt register is read again. */
  if (!(irq & IRQ_COMMAND_DONE))
    {
      if (++self->finger_poll_count >= FINGER_POLL_MAX)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "finger query not answered after %u polls "
                                                         "(last interrupt %02x)",
                                                         self->finger_poll_count,
                                                         irq));
          return;
        }

      fpi_ssm_jump_to_state_delayed (ssm, C_WAIT_OFF_STATUS, FINGER_POLL_DELAY);
      return;
    }

  fpi_ssm_next_state (ssm);
}

/* The zones decide whether the finger has been lifted; the interrupt bits
 * are not a finger proxy. */
static void
s7301_wait_off_zones_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  guint zones;
  guint count;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (len != 3)
    {
      fpi_ssm_mark_failed (ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "short zone reply (%zu bytes)",
                                                     len));
      return;
    }

  zones = s7301_decode_zones (resp);
  count = s7301_count_zones (zones);
  fp_dbg ("samsung7301: zones %03x (%u covered)", zones, count);

  if (count > DETECT_ZONES_OFF)
    {
      fpi_ssm_jump_to_state_delayed (ssm, C_WAIT_OFF, POLL_DELAY);
      return;
    }

  if (self->finger_present)
    {
      self->finger_present = FALSE;
      fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NONE);
    }

  fpi_ssm_next_state (ssm);
}

/* ── The result of the matcher ───────────────────────────────────────────── */

/* The frames that the matcher selected are added to the reference frames.
 * They are the frames that are stored with the print. */
static void
s7301_keep_selected_frames (FpiDeviceSamsung7301 *self)
{
  S7301HelperResult *result = &self->helper_result;
  guint i;

  for (i = 0; i < result->selected_count; i++)
    {
      g_ptr_array_add (self->refs,
                       g_memdup2 (g_ptr_array_index (self->frames, result->indices[i]),
                                  IMG_SIZE));
    }
}

/* The enrolment accepts a stage and asks for the next one. */
static void
s7301_enroll_accept (FpiDeviceSamsung7301 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  FpPrint *print = self->enroll_print;

  s7301_keep_selected_frames (self);
  self->enroll_stage++;

  /* The print carries the frames that were accepted so far. */
  g_object_set (print, "fpi-data", s7301_build_raw_data (self->refs), NULL);

  /* Only an accepted stage is reported as progress. */
  fpi_device_enroll_progress (dev, self->enroll_stage, print, NULL);

  if (self->enroll_stage >= ENROLL_STAGES)
    {
      self->enroll_print = print;
      fpi_ssm_mark_completed (self->ssm);
      return;
    }

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
  fpi_ssm_jump_to_state (self->ssm, C_DETECT_SETUP);
}

/* The enrolment has to try again: either the capture was too weak or the
 * finger was not the one that was enrolled before. */
static void
s7301_enroll_retry (FpiDeviceSamsung7301 *self)
{
  FpDevice *dev = FP_DEVICE (self);
  S7301HelperResult *result = &self->helper_result;
  GError *error;

  if (result->quality_max >= AFIS_THRESHOLD)
    error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                      "the finger does not match the images that were "
                                      "enrolled before, please use the same finger");
  else
    error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                      "the capture was too weak, please press the finger "
                                      "again");

  fpi_device_enroll_progress (dev, self->enroll_stage, NULL, error);

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);
  fpi_ssm_jump_to_state (self->ssm, C_DETECT_SETUP);
}

/* The verification: an adequate capture with a score below the threshold is
 * a real mismatch, an error is never a match. */
static void
s7301_verify_result (FpiDeviceSamsung7301 *self)
{
  S7301HelperResult *result = &self->helper_result;

  if (result->quality_max < AFIS_THRESHOLD)
    {
      self->verify_result = FPI_MATCH_ERROR;
      self->verify_error = fpi_device_retry_new_msg (FP_DEVICE_RETRY_GENERAL,
                                                      "the capture was too weak, please press "
                                                      "the finger again");
      return;
    }

  self->verify_error = NULL;
  self->verify_result = (result->best_cross >= AFIS_THRESHOLD) ? FPI_MATCH_SUCCESS
                                                               : FPI_MATCH_FAIL;
}

static void
s7301_result (FpiDeviceSamsung7301 *self)
{
  S7301HelperResult *result = &self->helper_result;

  fp_dbg ("samsung7301: matcher quality %g, best cross %g, %u frames selected",
          result->quality_max, result->best_cross, result->selected_count);

  if (self->enrolling)
    {
      if (result->selected_count == 0)
        s7301_enroll_retry (self);
      else
        s7301_enroll_accept (self);
      return;
    }

  s7301_verify_result (self);
  fpi_ssm_mark_completed (self->ssm);
}

static void
s7301_capture_loop (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (self->cancelled || fpi_device_action_is_cancelled (dev))
    {
      fpi_ssm_mark_failed (ssm,
                           g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                                "the operation was cancelled"));
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case C_DETECT_SETUP:
      s7301_exchange (dev, ssm, detect_gain, sizeof (detect_gain),
                      sizeof (detect_gain), FALSE, s7301_detect_setup_done);
      break;

    case C_ARM:
      s7301_exchange (dev, ssm, cmd_arm, sizeof (cmd_arm), 1, TRUE,
                      s7301_arm_done);
      break;

    case C_STATUS:
      s7301_exchange (dev, ssm, cmd_status, sizeof (cmd_status), 2, FALSE,
                      s7301_status_done);
      break;

    case C_ZONES:
      s7301_exchange (dev, ssm, cap_zones, sizeof (cap_zones), 3, FALSE,
                      s7301_zones_done);
      break;

    case C_SETTLE:
      fpi_ssm_next_state (ssm);
      break;

    case C_SETUP:
      /* A fresh press of the finger starts a new burst. */
      g_clear_pointer (&self->frames, g_ptr_array_unref);
      self->frames = g_ptr_array_new_with_free_func (g_free);
      self->seq_idx = 0;
      s7301_run_capture_seq (self);
      break;

    case C_TRIGGER:
      s7301_exchange (dev, ssm, cmd_trigger, sizeof (cmd_trigger), 1, TRUE,
                      s7301_trigger_done);
      break;

    case C_WAIT_IMAGE:
      s7301_exchange (dev, ssm, cmd_status, sizeof (cmd_status), 2, FALSE,
                      s7301_wait_image_done);
      break;

    case C_READ:
      s7301_read_image (self);
      break;

    case C_RESTORE_DETECT:
      s7301_exchange (dev, ssm, detect_gain, sizeof (detect_gain),
                      sizeof (detect_gain), FALSE, s7301_restore_detect_done);
      break;

    case C_WAIT_OFF:
      s7301_exchange (dev, ssm, cmd_arm, sizeof (cmd_arm), 1, TRUE,
                      s7301_wait_off_arm_done);
      break;

    case C_WAIT_OFF_STATUS:
      s7301_exchange (dev, ssm, cmd_status, sizeof (cmd_status), 2, FALSE,
                      s7301_wait_off_done);
      break;

    case C_WAIT_OFF_ZONES:
      s7301_exchange (dev, ssm, cap_zones, sizeof (cap_zones), 3, FALSE,
                      s7301_wait_off_zones_done);
      break;

    case C_HELPER:
      s7301_helper_start (self);
      break;

    case C_RESULT:
      s7301_result (self);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
s7301_capture_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  FpPrint *enroll_print = self->enroll_print;

  self->ssm = NULL;
  self->enroll_print = NULL;

  g_clear_pointer (&self->helper_timeout, g_source_destroy);
  s7301_helper_cleanup (self);
  g_clear_pointer (&self->frames, g_ptr_array_unref);
  g_clear_pointer (&self->refs, g_ptr_array_unref);

  if (self->cancelled || fpi_device_action_is_cancelled (dev))
    {
      GError *cancelled = NULL;

      if (error && g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        cancelled = error;
      else
        {
          g_clear_error (&error);
          cancelled = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                           "the operation was cancelled");
        }

      g_clear_pointer (&self->verify_error, g_error_free);

      if (self->enrolling)
        fpi_device_enroll_complete (dev, NULL, cancelled);
      else if (self->verifying)
        fpi_device_verify_complete (dev, cancelled);
      else
        g_error_free (cancelled);
      return;
    }

  if (self->enrolling)
    {
      g_clear_pointer (&self->verify_error, g_error_free);

      if (error)
        fpi_device_enroll_complete (dev, NULL, error);
      else if (enroll_print)
        fpi_device_enroll_complete (dev, g_object_ref (enroll_print), NULL);
      else
        fpi_device_enroll_complete (dev, NULL,
                                    fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  if (self->verifying)
    {
      if (error)
        {
          g_clear_pointer (&self->verify_error, g_error_free);
          fpi_device_verify_complete (dev, error);
          return;
        }

      /* The scanned print is not reported: it is a raw print of the burst
       * and would not be equal to the stored print. */
      fpi_device_verify_report (dev, self->verify_result, NULL,
                                g_steal_pointer (&self->verify_error));
      fpi_device_verify_complete (dev, NULL);
      return;
    }

  g_clear_error (&error);
  g_clear_pointer (&self->verify_error, g_error_free);
}

/* ── Enrolment ───────────────────────────────────────────────────────────── */

static void
s7301_dev_enroll (FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  FpPrint *print = NULL;

  fpi_device_get_enroll_data (dev, &print);
  if (!print)
    {
      fpi_device_enroll_complete (dev, NULL,
                                  fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  fpi_print_set_type (print, FPI_PRINT_RAW);

  self->enrolling = TRUE;
  self->verifying = FALSE;
  self->cancelled = FALSE;
  self->enroll_stage = 0;
  self->enroll_print = print;
  self->refs = g_ptr_array_new_with_free_func (g_free);
  self->frames = g_ptr_array_new_with_free_func (g_free);

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);

  self->ssm = fpi_ssm_new (dev, s7301_capture_loop, C_NUM_STATES);
  fpi_ssm_start (self->ssm, s7301_capture_complete);
}

/* ── Verification ────────────────────────────────────────────────────────── */

static void
s7301_dev_verify (FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  FpPrint *print = NULL;
  GVariant *data = NULL;
  GError *error = NULL;

  fpi_device_get_verify_data (dev, &print);
  if (!print)
    {
      fpi_device_verify_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }

  /* A print of an older layout -- in particular an NBIS print -- cannot be
   * used and needs a new enrolment. */
  if (fpi_print_get_type (print) != FPI_PRINT_RAW)
    {
      fpi_device_verify_complete (dev,
                                  fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_INVALID,
                                                            "the stored print is not a "
                                                            "\"%s\" print, please re-enrol "
                                                            "this finger",
                                                            RAW_PRINT_VERSION));
      return;
    }

  g_object_get (print, "fpi-data", &data, NULL);
  self->refs = s7301_parse_raw_data (data, &error);
  g_clear_pointer (&data, g_variant_unref);
  if (!self->refs)
    {
      fpi_device_verify_complete (dev, error);
      return;
    }

  self->enrolling = FALSE;
  self->verifying = TRUE;
  self->cancelled = FALSE;
  self->verify_result = FPI_MATCH_FAIL;
  self->verify_error = NULL;
  self->frames = g_ptr_array_new_with_free_func (g_free);

  fpi_device_report_finger_status (dev, FP_FINGER_STATUS_NEEDED);

  self->ssm = fpi_ssm_new (dev, s7301_capture_loop, C_NUM_STATES);
  fpi_ssm_start (self->ssm, s7301_capture_complete);
}

/* ── Cancellation ────────────────────────────────────────────────────────── */

static void
s7301_dev_cancel (FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  self->cancelled = TRUE;

  /* The transfers of the sensor are cancelled through the cancellable of the
   * device.  The helper is killed here, but nothing is reported before its
   * asynchronous completion arrived, so that the state machine is only freed
   * after the helper is really gone. */
  if (self->helper && !self->helper_done)
    g_subprocess_force_exit (self->helper);
}

/* ── Device class ────────────────────────────────────────────────────────── */

G_DEFINE_TYPE (FpiDeviceSamsung7301, fpi_device_samsung7301, FP_TYPE_DEVICE)

static void
fpi_device_samsung7301_init (FpiDeviceSamsung7301 *self)
{
  /* State machines are allocated per operation and freed by libfprint. */
}

static void
fpi_device_samsung7301_class_init (FpiDeviceSamsung7301Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id =              FP_COMPONENT;
  dev_class->full_name =       "Samsung 04e8:7301 Fingerprint Sensor";
  dev_class->type =            FP_DEVICE_TYPE_USB;
  dev_class->scan_type =       FP_SCAN_TYPE_PRESS;
  dev_class->id_table =        id_table;
  dev_class->nr_enroll_stages = ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open =            s7301_dev_open;
  dev_class->close =           s7301_dev_close;
  dev_class->enroll =          s7301_dev_enroll;
  dev_class->verify =          s7301_dev_verify;
  dev_class->cancel =          s7301_dev_cancel;

  /* The features are derived from the operations that are implemented, so
   * that the device advertises the verification it can do. */
  fpi_device_class_auto_initialize_features (dev_class);
}
