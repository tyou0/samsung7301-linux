#!/usr/bin/env python3
"""Compile and exercise the Samsung 7301 latency callbacks in isolation."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DRIVER = (ROOT / "contrib/samsung7301.c").read_text()


def function(name):
    pattern = re.compile(r"\bstatic\s+void\s+" + re.escape(name) + r"\s*\(")
    for match in pattern.finditer(DRIVER):
        brace = DRIVER.find("{", match.end())
        semicolon = DRIVER.find(";", match.end())
        if semicolon != -1 and semicolon < brace:
            continue
        depth = 0
        for end in range(brace, len(DRIVER)):
            if DRIVER[end] == "{":
                depth += 1
            elif DRIVER[end] == "}":
                depth -= 1
                if depth == 0:
                    return DRIVER[match.start():end + 1]
    raise AssertionError(f"could not find definition of {name}")


complete = function("s7301_capture_complete")
reset_start = complete.index("/* Verification completion reports finger-off")
reset_end = complete.index("  self->ssm = NULL;", reset_start)
finger_reset = complete[reset_start:reset_end]

cancel_check = complete.index("if (self->cancelled || fpi_device_action_is_cancelled")
verify_branch = complete.index("\n  if (self->verifying)\n    {", cancel_check)
verify_error = complete.index("fpi_device_verify_complete (dev, error);", verify_branch)
verify_report = complete.index("fpi_device_verify_report (dev,", verify_branch)
assert reset_start < cancel_check < verify_branch < verify_error < verify_report
assert "s7301_helper_cleanup (self);" in function("s7301_helper_maybe_finish")
helper_finish = function("s7301_helper_maybe_finish")
assert helper_finish.index("s7301_helper_cleanup (self);") < helper_finish.index(
    "fpi_ssm_next_state (self->ssm);"
)

support = r"""
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int guint;
typedef int gboolean;
typedef uint8_t guint8;
typedef size_t gsize;
typedef int64_t gint64;
typedef struct { guint len; } GPtrArray;
typedef struct { int action_cancelled; guint present_reports; } FpDevice;
typedef struct { int code; } GError;
typedef struct {
  guint cur_state, action, target, delay;
  GError *error;
} FpiSsm;
typedef enum { FPI_MATCH_FAIL, FPI_MATCH_SUCCESS, FPI_MATCH_ERROR } FpiMatchResult;
typedef struct {
  double quality_max, best_cross;
  guint selected_count;
  int indices[4];
} S7301HelperResult;
typedef struct {
  FpDevice parent;
  FpiSsm *ssm;
  GPtrArray *frames;
  gint64 next_trigger_at;
  S7301HelperResult helper_result;
  gboolean enrolling, verifying, cancelled, finger_present;
  FpiMatchResult verify_result;
  GError *verify_error;
} FpiDeviceSamsung7301;

enum { C_DETECT_SETUP, C_ARM, C_STATUS, C_ZONES, C_SETTLE, C_SETUP,
       C_TRIGGER, C_WAIT_IMAGE, C_READ, C_RESTORE_DETECT, C_WAIT_OFF,
       C_WAIT_OFF_STATUS, C_WAIT_OFF_ZONES, C_HELPER, C_RESULT };
enum { ACT_NONE, ACT_NEXT, ACT_JUMP, ACT_DELAYED, ACT_NEXT_DELAYED,
       ACT_COMPLETE, ACT_FAILED };
enum { FP_FINGER_STATUS_PRESENT };
enum { FP_DEVICE_RETRY_GENERAL = 1 };
#define TRUE 1
#define FALSE 0
#define FP_DEVICE(p) ((FpDevice *) (p))
#define FPI_DEVICE_SAMSUNG7301(p) ((FpiDeviceSamsung7301 *) (p))
#define AFIS_THRESHOLD 40.0
#define CAPTURE_FRAMES 16
#define VERIFY_EARLY_FRAMES 4
#define BURST_DELAY 80
#define CONTACT_SETTLE 150
#define DETECT_ZONES_ON 7
#define POLL_DELAY 30
#define G_IO_ERROR 1
#define G_IO_ERROR_CANCELLED 2
#define FP_DEVICE_ERROR 3
#define FP_DEVICE_ERROR_PROTO 4
#define G_TIME_SPAN_MILLISECOND 1000
#define fp_dbg(...) ((void) 0)

static gint64 monotonic_time;
static gint64 g_get_monotonic_time(void)
{ return monotonic_time; }

static GError *g_error_new_literal(int domain, int code, const char *message)
{ (void) domain; (void) message; GError *e = calloc(1, sizeof(*e)); e->code = code; return e; }
static GError *fpi_device_error_new_msg(int domain, const char *format, ...)
{ (void) format; return g_error_new_literal(domain, 0, "error"); }
static GError *fpi_device_retry_new_msg(int type, const char *format, ...)
{ (void) type; (void) format; return g_error_new_literal(0, 0, "retry"); }
static gboolean fpi_device_action_is_cancelled(FpDevice *dev)
{ return dev->action_cancelled; }
static void fpi_ssm_mark_failed(FpiSsm *ssm, GError *error)
{ ssm->action = ACT_FAILED; ssm->error = error; }
static void fpi_ssm_mark_completed(FpiSsm *ssm)
{ ssm->action = ACT_COMPLETE; }
static void fpi_ssm_next_state(FpiSsm *ssm)
{ ssm->action = ACT_NEXT; ssm->target = ssm->cur_state + 1; }
static void fpi_ssm_jump_to_state(FpiSsm *ssm, guint target)
{ ssm->action = ACT_JUMP; ssm->target = target; }
static void fpi_ssm_jump_to_state_delayed(FpiSsm *ssm, guint target, guint delay)
{ ssm->action = ACT_DELAYED; ssm->target = target; ssm->delay = delay; }
static void fpi_ssm_next_state_delayed(FpiSsm *ssm, guint delay)
{ ssm->action = ACT_NEXT_DELAYED; ssm->target = ssm->cur_state + 1; ssm->delay = delay; }
static void fpi_device_report_finger_status(FpDevice *dev, int status)
{ (void) status; dev->present_reports++; }
static guint s7301_decode_zones(const guint8 *resp)
{ return ((resp[1] << 8) | resp[2]) & 0xfff; }
static guint s7301_count_zones(guint zones)
{ guint count = 0; while (zones) { count += zones & 1; zones >>= 1; } return count; }
static guint enroll_accepts, enroll_retries;
static void s7301_enroll_accept(FpiDeviceSamsung7301 *self)
{ (void) self; enroll_accepts++; }
static void s7301_enroll_retry(FpiDeviceSamsung7301 *self)
{ (void) self; enroll_retries++; }
"""

tests = r"""
static void setup(FpiDeviceSamsung7301 *d, FpiSsm *ssm, GPtrArray *frames, guint n)
{
  monotonic_time = 1000000;
  *d = (FpiDeviceSamsung7301) { 0 };
  *ssm = (FpiSsm) { .cur_state = C_READ };
  *frames = (GPtrArray) { .len = n };
  d->ssm = ssm;
  d->frames = frames;
  d->verify_result = FPI_MATCH_FAIL;
}

static void test_burst_gates_helper_and_keeps_enrollment(void)
{
  FpiDeviceSamsung7301 d; FpiSsm ssm; GPtrArray frames;
  setup(&d, &ssm, &frames, 4); d.verifying = TRUE;
  s7301_burst_next(&d);
  assert(ssm.action == ACT_JUMP && ssm.target == C_HELPER);

  setup(&d, &ssm, &frames, 4); d.enrolling = TRUE;
  s7301_burst_next(&d);
  assert(ssm.action == ACT_DELAYED && ssm.target == C_TRIGGER && ssm.delay == BURST_DELAY);
  setup(&d, &ssm, &frames, 16); d.enrolling = TRUE;
  s7301_burst_next(&d);
  assert(ssm.action == ACT_NEXT && ssm.target == C_RESTORE_DETECT);
}

static void test_initial_success_and_fallback(void)
{
  FpiDeviceSamsung7301 d; FpiSsm ssm; GPtrArray frames;
  setup(&d, &ssm, &frames, 4); d.verifying = TRUE;
  d.helper_result.quality_max = 40; d.helper_result.best_cross = 40;
  s7301_result(&d);
  assert(d.verify_result == FPI_MATCH_SUCCESS && !d.verify_error);
  assert(ssm.action == ACT_JUMP && ssm.target == C_RESTORE_DETECT);

  setup(&d, &ssm, &frames, 8); d.verifying = TRUE;
  d.helper_result.quality_max = 40; d.helper_result.best_cross = 40;
  s7301_result(&d);
  assert(d.verify_result == FPI_MATCH_SUCCESS && !d.verify_error);
  assert(ssm.action == ACT_JUMP && ssm.target == C_RESTORE_DETECT);

  for (int weak = 0; weak < 2; weak++) {
    setup(&d, &ssm, &frames, 4); d.verifying = TRUE;
    d.helper_result.quality_max = weak ? 50 : 39;
    d.helper_result.best_cross = weak ? 39 : 90;
    s7301_burst_next(&d);
    assert(ssm.action == ACT_JUMP && ssm.target == C_HELPER);
    monotonic_time += 25500; /* 25.5 ms matching leaves 54.5 ms to round up. */
    s7301_result(&d);
    assert(ssm.action == ACT_DELAYED && ssm.target == C_TRIGGER && ssm.delay == 55);
    assert(frames.len == 4 && d.verify_result == FPI_MATCH_FAIL && !d.verify_error);
    for (frames.len = 5; frames.len < 16; frames.len++) {
      ssm.action = ACT_NONE;
      monotonic_time += 185000;
      s7301_burst_next(&d);
      if (frames.len % VERIFY_EARLY_FRAMES == 0) {
        /* An intermediate check that stays weak continues the burst. */
        assert(ssm.action == ACT_JUMP && ssm.target == C_HELPER);
        ssm.action = ACT_NONE;
        /* A later check gets a fresh interval, not the deadline from frame 4. */
        monotonic_time += frames.len == 8 ? 12500 : (weak ? 140000 : 80000);
        s7301_result(&d);
        assert(ssm.target == C_TRIGGER);
        if (frames.len == 8)
          assert(ssm.action == ACT_DELAYED && ssm.delay == 68);
        else
          assert(ssm.action == ACT_JUMP);
        assert(d.verify_result == FPI_MATCH_FAIL && !d.verify_error);
      } else {
        assert(ssm.action == ACT_DELAYED && ssm.target == C_TRIGGER);
      }
    }
    ssm.action = ACT_NONE;
    s7301_burst_next(&d);
    assert(ssm.action == ACT_JUMP && ssm.target == C_HELPER);
  }
}

static void test_final_decisions_and_restore(void)
{
  FpiDeviceSamsung7301 d; FpiSsm ssm; GPtrArray frames;
  setup(&d, &ssm, &frames, 16); d.verifying = TRUE;
  d.helper_result.quality_max = 50; d.helper_result.best_cross = 39;
  s7301_result(&d);
  assert(d.verify_result == FPI_MATCH_FAIL && !d.verify_error);
  assert(ssm.action == ACT_JUMP && ssm.target == C_RESTORE_DETECT);

  setup(&d, &ssm, &frames, 16); d.verifying = TRUE;
  d.helper_result.quality_max = 39; d.helper_result.best_cross = 90;
  s7301_result(&d);
  assert(d.verify_result == FPI_MATCH_ERROR && d.verify_error);
  assert(ssm.action == ACT_JUMP && ssm.target == C_RESTORE_DETECT);

  setup(&d, &ssm, &frames, 16); d.verifying = TRUE;
  s7301_restore_detect_done(&d.parent, &ssm, NULL, 0, NULL);
  assert(ssm.action == ACT_COMPLETE);
  setup(&d, &ssm, &frames, 16); d.enrolling = TRUE; ssm.cur_state = C_RESTORE_DETECT;
  s7301_restore_detect_done(&d.parent, &ssm, NULL, 0, NULL);
  assert(ssm.action == ACT_NEXT && ssm.target == C_WAIT_OFF);
  setup(&d, &ssm, &frames, 16); d.verifying = TRUE;
  s7301_restore_detect_done(&d.parent, &ssm, NULL, 0, g_error_new_literal(0, 0, "restore"));
  assert(ssm.action == ACT_FAILED);
}

static void test_completion_reset_allows_next_scan_to_report_present(void)
{
  FpiDeviceSamsung7301 d; FpiSsm ssm; GPtrArray frames;
  guint8 zones[3] = { 0, 0, 0x7f };
  setup(&d, &ssm, &frames, 4); d.verifying = TRUE; d.finger_present = TRUE;
  s7301_capture_completion_bookkeeping(&d); /* also runs on the cancellation path */
  assert(!d.finger_present);
  s7301_zones_done(&d.parent, &ssm, zones, sizeof(zones), NULL);
  assert(d.finger_present && d.parent.present_reports == 1);
  assert(ssm.action == ACT_NEXT_DELAYED && ssm.delay == CONTACT_SETTLE);
}

static void test_cancelled_burst_stops(void)
{
  FpiDeviceSamsung7301 d; FpiSsm ssm; GPtrArray frames;
  setup(&d, &ssm, &frames, 4); d.verifying = TRUE; d.parent.action_cancelled = TRUE;
  s7301_burst_next(&d);
  assert(ssm.action == ACT_FAILED && ssm.error && ssm.error->code == G_IO_ERROR_CANCELLED);
}

int main(void)
{
  test_burst_gates_helper_and_keeps_enrollment();
  test_initial_success_and_fallback();
  test_final_decisions_and_restore();
  test_completion_reset_allows_next_scan_to_report_present();
  test_cancelled_burst_stops();
  puts("Samsung 7301 latency state checks passed");
  return 0;
}
"""

source = "\n".join(
    [
        support,
        function("s7301_burst_next"),
        function("s7301_zones_done"),
        function("s7301_restore_detect_done"),
        function("s7301_verify_result"),
        function("s7301_result"),
        "static void s7301_capture_completion_bookkeeping(FpiDeviceSamsung7301 *self) {\n"
        + finger_reset
        + "}\n",
        tests,
    ]
)

with tempfile.TemporaryDirectory(prefix="samsung7301-latency-") as tmp:
    binary = Path(tmp) / "latency-check"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-x", "c", "-o", str(binary), "-"],
        input=source,
        text=True,
        check=True,
    )
    subprocess.run([str(binary)], check=True)

print("Samsung 7301 source-order checks passed")
