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

/* Pixels per millimetre.  The FPC1021 has an active area of roughly
 * 8.1 mm, which is 500 dpi. */
#define IMG_PPMM 19.69

/* NBIS is unreliable on very small images, so the frame is upscaled. */
#define RESIZE_FACTOR 2

/* bozorth3 match threshold.  The default of 40 is too strict for the noisy
 * frames this sensor produces. */
#define BZ3_THRESHOLD 15

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

/* Status byte of the 0x1c reply. */
#define STATUS_FINGER 0x81

/* ── Bring-up sequence ───────────────────────────────────────────────────── */

/* The FPC1021 configuration table of the Samsung Windows driver
 * (fpc_wbdi.dll), expressed as "register, value, ..." frames. */
static const guint8 init_adcsetup[]   = { 0x9c, 0x55, 0x40, 0x00, 0x2d, 0x24 };
static const guint8 init_pxlrstdly[]  = { 0x6c, 0x29, 0x00, 0x90, 0x20 };
static const guint8 init_sampledly[]  = { 0x68, 0x31, 0x31, 0x3d, 0x3d };
static const guint8 init_adcweight[]  = { 0x98, 0x03, 0xeb, 0x3f, 0xb8, 0x0a,
                                          0x70, 0x11, 0x22, 0x6a, 0x5d };
static const guint8 init_fngdrive[]   = { 0x8c, 0x12, 0x00, 0x5c, 0xb0 };
static const guint8 init_detthresh[]  = { 0xd8, 0x20, 0x00 };
static const guint8 init_detcntr[]    = { 0xdc, 0x00, 0x0a };
static const guint8 init_imagesetup[] = { 0x5c, 0x0b };
static const guint8 init_pxlctrl[]    = { 0xa8, 0x0f, 0x0a };
static const guint8 init_adcshift[]   = { 0xa0, 0x0a, 0x02 };
static const guint8 init_done[]       = { 0x28 };
static const guint8 init_status[]     = { 0x1c, 0x00 };
static const guint8 init_hwid[]       = { 0xfc, 0x00, 0x00 };
static const guint8 init_reset[]      = { 0xf8 };

/* Analogue front end settings for this chip revision (0x1f).  The driver's
 * values saturate the sensor; these were found by sweeping the registers. */
static const guint8 tune_gain[]       = { 0xa0, 0x00, 0x00 };
static const guint8 tune_adcsetup[]   = { 0x9c, 0x55, 0x40, 0x00, 0x08, 0x24 };
static const guint8 tune_imagerd[]    = { 0x64, 0x12 };

/* Single byte commands used by the capture state machine.  These have to
 * have static storage: the transfers are asynchronous, so the buffers have
 * to stay valid until the transfer completes. */
static const guint8 cmd_arm[]     = { OP_ARM };
static const guint8 cmd_status[]  = { OP_STATUS, 0x00 };
static const guint8 cmd_trigger[] = { OP_TRIGGER };

/* Capture sequence. */
static const guint8 cap_capgeo[]      = { 0x54, 0x00, 0xa0, 0x00, 0xa0 };
static const guint8 cap_capread[]     = { 0xd4, 0x00, 0x00, 0x00, 0x00 };

typedef struct
{
  const guint8 *data;
  gsize         len;
  guint16       reply_len;
  gboolean      doorbell;
} SamsungCmd;

#define CMD(d)    { (d), sizeof (d), sizeof (d), FALSE }
#define CMD_DB(d) { (d), sizeof (d), sizeof (d), TRUE }

static const SamsungCmd init_seq[] = {
  CMD_DB (init_reset),
  CMD    (init_status),
  CMD    (init_status),
  CMD    (init_hwid),
  CMD    (init_adcsetup),
  CMD    (init_pxlrstdly),
  CMD    (init_sampledly),
  CMD    (init_adcweight),
  CMD    (init_fngdrive),
  CMD    (init_detthresh),
  CMD    (init_detcntr),
  CMD    (init_imagesetup),
  CMD    (init_pxlctrl),
  CMD    (init_adcshift),
  CMD_DB (init_done),
};

static const SamsungCmd tuning_seq[] = {
  CMD (tune_gain),
  CMD (tune_adcsetup),
  CMD (tune_imagerd),
};

static const SamsungCmd capture_seq[] = {
  CMD (cap_capread),
  CMD (cap_capgeo),
};

/* ── Image helpers ───────────────────────────────────────────────────────── */

/* Linear contrast stretch: map the used range onto 0..255. */
static void
s7301_contrast_stretch (guint8 *data, gsize len)
{
  guint8 lo = 255, hi = 0;
  gsize i;

  if (!len)
    return;

  for (i = 0; i < len; i++)
    {
      lo = MIN (lo, data[i]);
      hi = MAX (hi, data[i]);
    }

  if (hi <= lo)
    return;

  for (i = 0; i < len; i++)
    data[i] = (guint8) ((data[i] - lo) * 255 / (hi - lo));
}

/* ── Driver state ────────────────────────────────────────────────────────── */

typedef struct _FpiDeviceSamsung7301 FpiDeviceSamsung7301;

struct _FpiDeviceSamsung7301
{
  FpImageDevice parent;

  FpiSsm  *open_ssm;
  FpiSsm  *ssm;

  guint   session_idx;
  guint   init_idx;
  guint   tune_idx;
  guint   seq_idx;
  guint8 *img_buf;
  guint image_pending;
  GError *image_error;
  gboolean deactivating;
  gboolean running;
  gboolean finger_present;
};

G_DECLARE_FINAL_TYPE (FpiDeviceSamsung7301, fpi_device_samsung7301, FPI,
                      DEVICE_SAMSUNG7301, FpImageDevice)

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
  O_INIT,
  O_TUNE,
  O_NUM_STATES,
};

static void s7301_run_session (FpDevice *dev, FpiSsm *ssm);
static void s7301_run_init (FpDevice *dev, FpiSsm *ssm);
static void s7301_run_tune (FpDevice *dev, FpiSsm *ssm);

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

static void
s7301_init_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
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
s7301_tune_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  s7301_run_tune (dev, ssm);
}

static void
s7301_run_tune (FpDevice *dev, FpiSsm *ssm)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);
  const SamsungCmd *c;

  if (self->tune_idx >= G_N_ELEMENTS (tuning_seq))
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  c = &tuning_seq[self->tune_idx++];
  s7301_exchange (dev, ssm, c->data, c->len, c->reply_len, c->doorbell,
                  s7301_tune_done);
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

    case O_INIT:
      self->init_idx = 0;
      s7301_run_init (dev, ssm);
      break;

    case O_TUNE:
      self->tune_idx = 0;
      s7301_run_tune (dev, ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
s7301_open_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FPI_DEVICE_SAMSUNG7301 (dev)->open_ssm = NULL;
  fpi_image_device_open_complete (FP_IMAGE_DEVICE (dev), error);
}

static void
s7301_dev_open (FpImageDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  self->session_idx = 0;
  self->init_idx = 0;
  self->tune_idx = 0;
  self->open_ssm = fpi_ssm_new (FP_DEVICE (dev), s7301_open_loop, O_NUM_STATES);
  fpi_ssm_start (self->open_ssm, s7301_open_complete);
}

static void
s7301_dev_close (FpImageDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  g_clear_pointer (&self->img_buf, g_free);
  fpi_image_device_close_complete (dev, NULL);
}

/* ── Capture ─────────────────────────────────────────────────────────────── */

enum {
  M_START,
  M_ARM,
  M_STATUS,
  M_SETUP,
  M_TRIGGER,
  M_READ,
  M_WAIT_OFF,
  M_NUM_STATES,
};

static void s7301_run_capture_seq (FpiDeviceSamsung7301 *self);
static void s7301_read_image (FpiDeviceSamsung7301 *self);

static void
s7301_arm_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
s7301_status_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("samsung7301: status %02x %02x", len > 0 ? resp[0] : 0,
          len > 1 ? resp[1] : 0);

  if (len >= 2 && resp[1] == STATUS_FINGER)
    {
      if (!self->finger_present)
        {
          self->finger_present = TRUE;
          fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);
        }
      fpi_ssm_next_state (ssm);
      return;
    }

  fpi_ssm_jump_to_state_delayed (ssm, M_ARM, POLL_DELAY);
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
  fpi_ssm_next_state (ssm);
}

/* The image read needs the bulk IN to be armed before the bulk OUT is
 * written, so it does not use the sequential exchange above. */
static void s7301_img_after_read (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);
static void s7301_img_after_write (FpiUsbTransfer *t, FpDevice *dev, gpointer ud, GError *err);

/* The image read is bracketed by the same two control transfers as every
 * other command (heartbeat, then the 0xca that primes the reply length), and
 * then the bulk IN has to be armed before the bulk OUT is written. */
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

/* Both transfers must finish before the SSM can advance or be freed. */
static void
s7301_img_transfer_done (FpiDeviceSamsung7301 *self, GError *err)
{
  FpImage *tmp;
  FpImage *img;

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

  tmp = fp_image_new (IMG_WIDTH, IMG_HEIGHT);
  tmp->ppmm = IMG_PPMM;
  memcpy (tmp->data, self->img_buf + IMG_HEADER, IMG_SIZE);

  s7301_contrast_stretch (tmp->data, (gsize) tmp->width * tmp->height);

  img = fpi_image_resize (tmp, RESIZE_FACTOR, RESIZE_FACTOR);
  g_object_unref (tmp);
  img->ppmm = IMG_PPMM * RESIZE_FACTOR;

  fpi_image_device_image_captured (FP_IMAGE_DEVICE (self), img);

  /* Wait for the finger to be lifted before the next scan. */
  fpi_ssm_next_state (self->ssm);
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

static void
s7301_wait_off_done (FpDevice *dev, FpiSsm *ssm, const guint8 *resp, gsize len, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (len >= 2 && resp[1] != STATUS_FINGER)
    {
      if (self->finger_present)
        {
          self->finger_present = FALSE;
          fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), FALSE);
        }
      fpi_ssm_jump_to_state_delayed (ssm, M_ARM, POLL_DELAY);
      return;
    }

  fpi_ssm_jump_to_state_delayed (ssm, M_WAIT_OFF, POLL_DELAY);
}

static void
s7301_capture_loop (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_START:
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), NULL);
      fpi_ssm_next_state (ssm);
      break;

    case M_ARM:
      s7301_exchange (dev, ssm, cmd_arm, sizeof (cmd_arm), 1, TRUE,
                      s7301_arm_done);
      break;

    case M_STATUS:
      s7301_exchange (dev, ssm, cmd_status, sizeof (cmd_status), 2, FALSE,
                      s7301_status_done);
      break;

    case M_SETUP:
      self->seq_idx = 0;
      s7301_run_capture_seq (self);
      break;

    case M_TRIGGER:
      s7301_exchange (dev, ssm, cmd_trigger, sizeof (cmd_trigger), 1, TRUE,
                      s7301_trigger_done);
      break;

    case M_READ:
      s7301_read_image (self);
      break;

    case M_WAIT_OFF:
      s7301_exchange (dev, ssm, cmd_status, sizeof (cmd_status), 2, FALSE,
                      s7301_wait_off_done);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
s7301_capture_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  self->ssm = NULL;
  self->running = FALSE;

  if (self->deactivating)
    fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (dev), error);
  else if (error)
    fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
}

static void
s7301_dev_activate (FpImageDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  self->deactivating = FALSE;
  self->finger_present = FALSE;
  self->running = TRUE;
  /* The frames this sensor produces are noisy, so the default bozorth3
   * threshold of 40 rejects every genuine match. */
  fpi_image_device_set_bz3_threshold (dev, BZ3_THRESHOLD);
  self->ssm = fpi_ssm_new (FP_DEVICE (dev), s7301_capture_loop, M_NUM_STATES);
  fpi_ssm_start (self->ssm, s7301_capture_complete);
}

static void
s7301_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceSamsung7301 *self = FPI_DEVICE_SAMSUNG7301 (dev);

  self->deactivating = TRUE;
  if (!self->running)
    {
      fpi_image_device_deactivate_complete (dev, NULL);
      return;
    }
  /* The state machine notices the flag at the next transition; the polling
   * delays are short, so this happens right away. */
}

/* ── Device class ────────────────────────────────────────────────────────── */

G_DEFINE_TYPE (FpiDeviceSamsung7301, fpi_device_samsung7301, FP_TYPE_IMAGE_DEVICE)

static void
fpi_device_samsung7301_init (FpiDeviceSamsung7301 *self)
{
  /* State machines are allocated per operation and freed by libfprint. */
}

static void
fpi_device_samsung7301_class_init (FpiDeviceSamsung7301Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id =              FP_COMPONENT;
  dev_class->full_name =       "Samsung 04e8:7301 Fingerprint Sensor";
  dev_class->type =            FP_DEVICE_TYPE_USB;
  dev_class->scan_type =       FP_SCAN_TYPE_PRESS;
  dev_class->id_table =        id_table;
  dev_class->nr_enroll_stages = 5;
  dev_class->temp_hot_seconds = -1;

  img_class->img_open =        s7301_dev_open;
  img_class->img_close =       s7301_dev_close;
  img_class->activate =        s7301_dev_activate;
  img_class->deactivate =      s7301_dev_deactivate;
}
