/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * pam_fprint_parallel: one prompt that accepts either the password or an
 * enrolled fingerprint, whichever comes first.
 *
 * The module never checks a password. A typed password becomes PAM_AUTHTOK
 * and the module returns PAM_IGNORE, so the following pam_unix
 * (try_first_pass) decides. Only fprintd's "verify-match" returns
 * PAM_SUCCESS. When the fingerprint cannot be used (remote session, piped
 * stdin, no reader or no enrolled finger) the module returns PAM_IGNORE
 * without prompting and the usual password prompt follows.
 *
 *   -auth [success=ok ignore=1 conv_err=die default=1] pam_fprint_parallel.so mode=tty
 *   auth  [default=done] pam_faillock.so authsucc
 *
 * A match continues to the faillock reset, which ends the stack; anything
 * else, including a missing module, skips it to the password stack. Never use
 * default=die: a missing module would then fail the whole service.
 *
 * mode=tty         Read the password from the controlling terminal while
 *                  fprintd verifies (sudo). Echo is off; the terminal and
 *                  signal handlers are restored on every path, and Ctrl+C is
 *                  re-raised to the application unchanged.
 * mode=conv        Ask through the PAM conversation in a helper thread
 *                  (polkit's agent helper). Only for applications that exit
 *                  after authentication: a fingerprint match leaves that
 *                  thread blocked in the conversation. polkit-agent-helper-1
 *                  reports the result first; it exits when the agent, having
 *                  read SUCCESS, closes the connection and the read ends.
 *                  Status messages are sent as PAM_TEXT_INFO from the main
 *                  thread while the prompt is pending, so the application's
 *                  conversation function must accept a message from another
 *                  thread during a prompt. The helper does: it only writes
 *                  them to stdout. KDE's dialog ignores the prompt text but
 *                  shows these messages.
 * timeout=N        Seconds the fingerprint stays armed; 0 (default) keeps it
 *                  armed until the prompt is answered.
 * max-tries=N      Non-matching fingerprints before only the password is
 *                  accepted (default 3).
 * lock-session=auto|none|ID
 *                  Release the reader while this logind session is locked,
 *                  so the lock screen can use it; re-arm after unlocking.
 *                  auto (default) uses the caller's session or, failing
 *                  that, the user's graphical session.
 * bus=ADDRESS      System bus address (tests). The environment is never used.
 * debug            Log decisions to syslog.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <security/pam_ext.h>
#include <security/pam_modules.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>

#define EXPORT __attribute__((visibility ("default")))

#define FPRINT_NAME "net.reactivated.Fprint"
#define FPRINT_MANAGER_PATH "/net/reactivated/Fprint/Manager"
#define FPRINT_MANAGER_IFACE "net.reactivated.Fprint.Manager"
#define FPRINT_DEVICE_IFACE "net.reactivated.Fprint.Device"
#define FPRINT_ERROR_BUSY "net.reactivated.Fprint.Error.AlreadyInUse"
#define LOGIN_NAME "org.freedesktop.login1"
#define SYSTEM_BUS "unix:path=/run/dbus/system_bus_socket"

#define USEC_PER_SEC 1000000ULL
#define CALL_TIMEOUT_USEC (15 * USEC_PER_SEC)
#define CLAIM_RETRY_USEC (2 * USEC_PER_SEC)
#define PASSWORD_MAX 512

typedef enum {
  MODE_TTY,
  MODE_CONV,
} Mode;

typedef enum {
  FP_OFF,       /* not usable for the rest of this prompt */
  FP_ARMED,     /* claimed and verifying */
  FP_BUSY,      /* another client holds the reader; retry the claim */
  FP_LOCKED,    /* released while the session is locked */
} FpState;

typedef enum {
  ACT_NONE,
  ACT_RESTART,
  ACT_DISARM,
} Action;

typedef enum {
  RES_MATCH,
  RES_PASSWORD,
  RES_SIGNAL,
  RES_CONV_ERROR,
} Result;

/* Shared with the conversation thread, which may outlive the module call.
 * The eventfd belongs to this struct: writing to it can never raise SIGPIPE,
 * and it is closed only by the last reference. */
typedef struct
{
  atomic_int      refs;
  int             notify_fd;
  struct pam_conv conv;
  char           *prompt;
  char           *answer;
  int             status;
} ConvShared;

typedef struct
{
  pam_handle_t  *pamh;
  const char    *user;
  const char    *service;
  Mode           mode;
  unsigned       timeout_s;
  unsigned       max_tries;
  const char    *bus_address;
  const char    *lock_session;
  bool           debug;
  bool           silent;

  sd_bus        *bus;
  char          *device;
  char          *fprintd_owner; /* unique bus name currently owning net.reactivated.Fprint */
  char          *logind_owner;
  bool           swipe;
  sd_bus_slot   *verify_slot;
  sd_bus_slot   *lock_slot;
  FpState        fp;
  bool           claimed;
  bool           verifying;
  unsigned       tries;
  uint64_t       deadline;
  uint64_t       retry_claim_at;
  bool           matched;
  Action         action;
  bool           lock_changed;
  bool           locked;

  int            tty;
  bool           tty_raw;
  struct termios saved_termios;
  char           prompt[256];
  char           password[PASSWORD_MAX + 1];
  size_t         password_len;

  ConvShared    *shared;
  int            notify_read;
  bool           conv_done;
} Ctx;

static volatile sig_atomic_t got_signal;
static const int caught_signals[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP };
static struct sigaction saved_actions[sizeof (caught_signals) / sizeof (caught_signals[0])];

static uint64_t
now_usec (void)
{
  struct timespec ts;

  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * USEC_PER_SEC + (uint64_t) ts.tv_nsec / 1000;
}

static void debug_log (Ctx        *c,
                       const char *format,
                       ...) __attribute__((format (printf, 2, 3)));

static void
debug_log (Ctx *c, const char *format, ...)
{
  va_list args;

  if (!c->debug)
    return;
  va_start (args, format);
  pam_vsyslog (c->pamh, LOG_DEBUG, format, args);
  va_end (args);
}

/* ---- output ---- */

static void
write_all (int fd, const char *text)
{
  size_t length = strlen (text);

  while (length > 0)
    {
      ssize_t written = write (fd, text, length);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        return;
      text += written;
      length -= written;
    }
}

static void
notify (Ctx *c, const char *message)
{
  if (c->silent)
    return;
  if (c->mode == MODE_CONV)
    {
      pam_info (c->pamh, "%s", message);
      return;
    }
  if (c->tty < 0)
    return;
  write_all (c->tty, "\n");
  write_all (c->tty, message);
  write_all (c->tty, "\n");
  write_all (c->tty, c->prompt);
}

/* ---- fprintd ---- */

/* Any client on the system bus can send a signal straight to our connection,
 * whatever our match rules say, and claim any sender path or interface. Only
 * the unique name that owns a privileged well-known name proves where a signal
 * came from, so matches and callbacks compare against that. */
static char *
name_owner (Ctx *c, const char *name)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *owner = NULL;
  char *result = NULL;

  if (sd_bus_call_method (c->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                          "org.freedesktop.DBus", "GetNameOwner", &error, &reply, "s", name) >= 0 &&
      sd_bus_message_read (reply, "s", &owner) >= 0 && owner[0] == ':')
    result = strdup (owner);
  sd_bus_error_free (&error);
  sd_bus_message_unref (reply);
  return result;
}

static bool
sent_by (sd_bus_message *message, const char *owner)
{
  const char *sender = sd_bus_message_get_sender (message);

  return owner && sender && strcmp (sender, owner) == 0;
}

static int
call (Ctx *c, const char *path, const char *iface, const char *method,
      sd_bus_error *error, sd_bus_message **reply, const char *types, ...)
{
  sd_bus_message *message = NULL;
  va_list args;
  int r;

  r = sd_bus_message_new_method_call (c->bus, &message, FPRINT_NAME, path, iface, method);
  if (r >= 0 && types)
    {
      va_start (args, types);
      r = sd_bus_message_appendv (message, types, args);
      va_end (args);
    }
  if (r >= 0)
    r = sd_bus_call (c->bus, message, CALL_TIMEOUT_USEC, error, reply);
  sd_bus_message_unref (message);
  return r;
}

static void
fp_release (Ctx *c)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;

  if (!c->bus)
    return;
  if (c->verifying)
    call (c, c->device, FPRINT_DEVICE_IFACE, "VerifyStop", &error, NULL, NULL);
  sd_bus_error_free (&error);
  if (c->claimed)
    call (c, c->device, FPRINT_DEVICE_IFACE, "Release", &error, NULL, NULL);
  sd_bus_error_free (&error);
  c->verifying = false;
  c->claimed = false;
}

static void
fp_off (Ctx *c)
{
  fp_release (c);
  c->fp = FP_OFF;
}

static int
fp_verify_start (Ctx *c)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int r = call (c, c->device, FPRINT_DEVICE_IFACE, "VerifyStart", &error, NULL, "s", "any");

  if (r < 0)
    debug_log (c, "VerifyStart failed: %s", error.message);
  sd_bus_error_free (&error);
  c->verifying = r >= 0;
  return r;
}

/* Claim the reader and start verifying. A reader held by another client
 * (the lock screen, another prompt) is retried, not given up. */
static void
fp_arm (Ctx *c)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  int r = call (c, c->device, FPRINT_DEVICE_IFACE, "Claim", &error, NULL, "s", c->user);

  if (r < 0)
    {
      bool busy = sd_bus_error_has_name (&error, FPRINT_ERROR_BUSY);
      debug_log (c, "Claim failed: %s", error.message);
      sd_bus_error_free (&error);
      if (busy)
        {
          c->fp = FP_BUSY;
          c->retry_claim_at = now_usec () + CLAIM_RETRY_USEC;
        }
      else
        {
          c->fp = FP_OFF;
        }
      return;
    }
  c->claimed = true;
  if (fp_verify_start (c) < 0)
    {
      fp_off (c);
      return;
    }
  c->fp = FP_ARMED;
}

static const char *
retry_message (Ctx *c, const char *result)
{
  if (strcmp (result, "verify-retry-scan") == 0)
    return c->swipe ? "Swipe your finger again" : "Place your finger on the reader again";
  if (strcmp (result, "verify-swipe-too-short") == 0)
    return "Swipe was too short, try again";
  if (strcmp (result, "verify-finger-not-centered") == 0)
    return "Your finger was not centered, try again";
  if (strcmp (result, "verify-remove-and-retry") == 0)
    return "Remove your finger, and try again";
  return NULL;
}

static int
on_verify_status (sd_bus_message *message, void *userdata, sd_bus_error *ret_error)
{
  Ctx *c = userdata;
  const char *result = NULL;
  int done = 0;
  const char *retry;

  if (!sent_by (message, c->fprintd_owner))
    {
      pam_syslog (c->pamh, LOG_WARNING, "ignoring VerifyStatus from %s, not fprintd",
                  sd_bus_message_get_sender (message) ? sd_bus_message_get_sender (message) : "(unknown)");
      return 0;
    }
  if (sd_bus_message_read (message, "sb", &result, &done) < 0 || c->fp != FP_ARMED)
    return 0;
  debug_log (c, "VerifyStatus %s", result);
  /* A finished verification still needs VerifyStop before the next start or
   * Release, so c->verifying stays set until fp_release or a restart. */

  if (strcmp (result, "verify-match") == 0)
    {
      c->matched = true;
    }
  else if (strcmp (result, "verify-no-match") == 0)
    {
      if (++c->tries >= c->max_tries)
        {
          notify (c, "Fingerprint not recognized; type your password");
          c->action = ACT_DISARM;
        }
      else
        {
          notify (c, "Fingerprint not recognized, try again");
          c->action = ACT_RESTART;
        }
    }
  else if ((retry = retry_message (c, result)))
    {
      notify (c, retry);
      if (done)
        c->action = ACT_RESTART;
    }
  else
    {
      /* verify-disconnected, verify-unknown-error or anything new. */
      pam_syslog (c->pamh, LOG_NOTICE, "fingerprint verification stopped: %s", result);
      c->action = ACT_DISARM;
    }
  return 0;
}

/* ---- logind lock state ---- */

static int
on_session_properties (sd_bus_message *message, void *userdata, sd_bus_error *ret_error)
{
  Ctx *c = userdata;
  const char *iface = NULL;

  if (!sent_by (message, c->logind_owner) ||
      sd_bus_message_read (message, "s", &iface) < 0 ||
      sd_bus_message_enter_container (message, 'a', "{sv}") < 0)
    return 0;
  while (sd_bus_message_enter_container (message, 'e', "sv") > 0)
    {
      const char *name = NULL;
      int locked = 0;
      if (sd_bus_message_read (message, "s", &name) >= 0 && strcmp (name, "LockedHint") == 0 &&
          sd_bus_message_read (message, "v", "b", &locked) >= 0)
        {
          c->locked = locked;
          c->lock_changed = true;
        }
      else
        {
          sd_bus_message_skip (message, "v");
        }
      sd_bus_message_exit_container (message);
    }
  return 0;
}

static char *
lock_session_id (Ctx *c)
{
  char *session = NULL;
  struct passwd *pw;

  if (strcmp (c->lock_session, "none") == 0)
    return NULL;
  if (strcmp (c->lock_session, "auto") != 0)
    return strdup (c->lock_session);
  /* Desktop terminals run under the user manager, outside any session. */
  if (sd_pid_get_session (0, &session) >= 0)
    return session;
  pw = getpwnam (c->user);
  if (pw && sd_uid_get_display (pw->pw_uid, &session) >= 0)
    return session;
  return NULL;
}

static void
watch_lock (Ctx *c)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *path = NULL;
  char *session = lock_session_id (c);
  int locked = 0;

  if (!session)
    return;
  if (sd_bus_call_method (c->bus, LOGIN_NAME, "/org/freedesktop/login1",
                          "org.freedesktop.login1.Manager", "GetSession", &error, &reply,
                          "s", session) >= 0 &&
      sd_bus_message_read (reply, "o", &path) >= 0 &&
      (c->logind_owner = name_owner (c, LOGIN_NAME)) &&
      sd_bus_match_signal (c->bus, &c->lock_slot, c->logind_owner, path,
                           "org.freedesktop.DBus.Properties", "PropertiesChanged",
                           on_session_properties, c) >= 0)
    {
      sd_bus_error_free (&error);
      if (sd_bus_get_property_trivial (c->bus, LOGIN_NAME, path, "org.freedesktop.login1.Session",
                                       "LockedHint", &error, 'b', &locked) >= 0)
        c->locked = locked;
      debug_log (c, "watching lock state of session %s", session);
    }
  sd_bus_error_free (&error);
  sd_bus_message_unref (reply);
  free (session);
}

/* Connect and check that a fingerprint can be used at all. */
static bool
fp_probe (Ctx *c)
{
  sd_bus_error error = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = NULL;
  const char *path = NULL;
  const char *scan_type = NULL;
  bool enrolled = false;

  if (sd_bus_new (&c->bus) < 0 ||
      sd_bus_set_address (c->bus, c->bus_address) < 0 ||
      sd_bus_set_bus_client (c->bus, 1) < 0 ||
      sd_bus_start (c->bus) < 0)
    {
      debug_log (c, "cannot connect to %s", c->bus_address);
      return false;
    }
  sd_bus_set_method_call_timeout (c->bus, CALL_TIMEOUT_USEC);

  if (call (c, FPRINT_MANAGER_PATH, FPRINT_MANAGER_IFACE, "GetDefaultDevice", &error, &reply, NULL) < 0 ||
      sd_bus_message_read (reply, "o", &path) < 0)
    {
      debug_log (c, "no fingerprint reader: %s", error.message);
      goto out;
    }
  c->device = strdup (path);
  reply = sd_bus_message_unref (reply);
  if (!c->device)
    goto out;

  if (call (c, c->device, FPRINT_DEVICE_IFACE, "ListEnrolledFingers", &error, &reply, "s", c->user) >= 0 &&
      sd_bus_message_enter_container (reply, 'a', "s") >= 0)
    enrolled = sd_bus_message_at_end (reply, false) == 0;
  if (!enrolled)
    {
      debug_log (c, "no enrolled fingerprint for %s", c->user);
      goto out;
    }
  sd_bus_error_free (&error);
  reply = sd_bus_message_unref (reply);
  /* sd_bus_get_property_* rejects fprintd's hyphenated property names as
   * invalid member names; ask org.freedesktop.DBus.Properties directly. */
  if (sd_bus_call_method (c->bus, FPRINT_NAME, c->device, "org.freedesktop.DBus.Properties", "Get",
                          &error, &reply, "ss", FPRINT_DEVICE_IFACE, "scan-type") >= 0 &&
      sd_bus_message_read (reply, "v", "s", &scan_type) >= 0)
    c->swipe = strcmp (scan_type, "swipe") == 0;
  else
    debug_log (c, "scan-type unavailable: %s", error.message ? error.message : "unexpected reply");

  /* GetDefaultDevice has started fprintd if it was not running. */
  c->fprintd_owner = name_owner (c, FPRINT_NAME);
  if (!c->fprintd_owner ||
      sd_bus_match_signal (c->bus, &c->verify_slot, c->fprintd_owner, c->device, FPRINT_DEVICE_IFACE,
                           "VerifyStatus", on_verify_status, c) < 0)
    {
      enrolled = false;
      goto out;
    }
  watch_lock (c);

out:
  sd_bus_error_free (&error);
  sd_bus_message_unref (reply);
  return enrolled;
}

/* ---- terminal input ---- */

static void
on_signal (int signo)
{
  got_signal = signo;
}

static bool
tty_begin (Ctx *c)
{
  struct termios raw;
  struct sigaction action = { .sa_handler = on_signal };

  if (tcgetattr (c->tty, &c->saved_termios) < 0)
    return false;
  raw = c->saved_termios;
  raw.c_lflag &= ~(ECHO | ECHONL | ICANON);
  raw.c_lflag |= ISIG;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  raw.c_cc[VSUSP] = _POSIX_VDISABLE;

  got_signal = 0;
  sigemptyset (&action.sa_mask);
  for (size_t i = 0; i < sizeof (caught_signals) / sizeof (caught_signals[0]); i++)
    sigaction (caught_signals[i], &action, &saved_actions[i]);
  /* Keep type-ahead: users often start typing before the prompt appears. */
  if (tcsetattr (c->tty, TCSADRAIN, &raw) < 0)
    {
      for (size_t i = 0; i < sizeof (caught_signals) / sizeof (caught_signals[0]); i++)
        sigaction (caught_signals[i], &saved_actions[i], NULL);
      return false;
    }
  c->tty_raw = true;
  write_all (c->tty, c->prompt);
  return true;
}

static void
tty_end (Ctx *c)
{
  if (!c->tty_raw)
    return;
  tcsetattr (c->tty, TCSADRAIN, &c->saved_termios);
  write_all (c->tty, "\n");
  for (size_t i = 0; i < sizeof (caught_signals) / sizeof (caught_signals[0]); i++)
    sigaction (caught_signals[i], &saved_actions[i], NULL);
  c->tty_raw = false;
}

static bool
fingerprint_pending (Ctx *c)
{
  return c->fp != FP_OFF;
}

/* Returns true when a password line is complete. */
static bool
tty_input (Ctx *c)
{
  char buffer[64];
  ssize_t length = read (c->tty, buffer, sizeof (buffer));

  for (ssize_t i = 0; i < length; i++)
    {
      unsigned char byte = buffer[i];
      if (byte == '\r' || byte == '\n' || byte == 0x04)
        {
          /* An empty line cannot be the password while a finger may still
           * arrive; it is usually a habitual Enter. */
          if (c->password_len == 0 && fingerprint_pending (c))
            continue;
          return true;
        }
      if (byte == 0x7f || byte == 0x08)
        {
          while (c->password_len > 0 && (c->password[c->password_len - 1] & 0xc0) == 0x80)
            c->password_len--;
          if (c->password_len > 0)
            c->password_len--;
        }
      else if (byte == 0x15 || byte == 0x17)
        {
          c->password_len = 0;
        }
      else if (byte >= 0x20 && c->password_len < PASSWORD_MAX)
        {
          c->password[c->password_len++] = byte;
        }
    }
  return false;
}

/* ---- conversation input ---- */

static void
shared_unref (ConvShared *shared)
{
  if (atomic_fetch_sub (&shared->refs, 1) != 1)
    return;
  if (shared->answer)
    {
      explicit_bzero (shared->answer, strlen (shared->answer));
      free (shared->answer);
    }
  if (shared->notify_fd >= 0)
    close (shared->notify_fd);
  free (shared->prompt);
  free (shared);
}

static void *
conv_thread (void *data)
{
  ConvShared *shared = data;
  struct pam_message message = { PAM_PROMPT_ECHO_OFF, shared->prompt };
  const struct pam_message *messages = &message;
  struct pam_response *response = NULL;
  int status = shared->conv.conv (1, &messages, &response, shared->conv.appdata_ptr);

  if (status == PAM_SUCCESS && response && response->resp)
    {
      shared->answer = response->resp;
    }
  else if (response && response->resp)
    {
      explicit_bzero (response->resp, strlen (response->resp));
      free (response->resp);
    }
  if (status == PAM_SUCCESS && !shared->answer)
    status = PAM_CONV_ERR;
  free (response);
  shared->status = status;
  atomic_thread_fence (memory_order_release);
  {
    uint64_t one = 1;
    ssize_t ignored = write (shared->notify_fd, &one, sizeof (one));
    (void) ignored;
  }
  shared_unref (shared);
  return NULL;
}

static bool
conv_begin (Ctx *c)
{
  const struct pam_conv *conv = NULL;
  int notify_fd;
  pthread_t thread;
  pthread_attr_t attributes;

  if (pam_get_item (c->pamh, PAM_CONV, (const void **) &conv) != PAM_SUCCESS || !conv || !conv->conv ||
      (notify_fd = eventfd (0, EFD_CLOEXEC)) < 0)
    return false;
  c->shared = calloc (1, sizeof (*c->shared));
  if (!c->shared || !(c->shared->prompt = strdup (c->prompt)))
    {
      free (c->shared);
      c->shared = NULL;
      close (notify_fd);
      return false;
    }
  atomic_init (&c->shared->refs, 2);
  c->shared->notify_fd = notify_fd;
  c->shared->conv = *conv;
  c->notify_read = notify_fd; /* borrowed from c->shared */
  pthread_attr_init (&attributes);
  pthread_attr_setdetachstate (&attributes, PTHREAD_CREATE_DETACHED);
  if (pthread_create (&thread, &attributes, conv_thread, c->shared) != 0)
    {
      pthread_attr_destroy (&attributes);
      atomic_store (&c->shared->refs, 1);
      shared_unref (c->shared);
      c->shared = NULL;
      return false;
    }
  pthread_attr_destroy (&attributes);
  return true;
}

/* ---- main loop ---- */

static void
apply_events (Ctx *c)
{
  uint64_t now = now_usec ();
  Action action = c->action;

  c->action = ACT_NONE;
  if (action == ACT_DISARM)
    {
      fp_off (c);
    }
  else if (action == ACT_RESTART && c->fp == FP_ARMED)
    {
      sd_bus_error error = SD_BUS_ERROR_NULL;
      if (c->verifying)
        call (c, c->device, FPRINT_DEVICE_IFACE, "VerifyStop", &error, NULL, NULL);
      sd_bus_error_free (&error);
      c->verifying = false;
      if (fp_verify_start (c) < 0)
        fp_off (c);
    }

  if (c->lock_changed)
    {
      c->lock_changed = false;
      if (c->locked && (c->fp == FP_ARMED || c->fp == FP_BUSY))
        {
          debug_log (c, "session locked; releasing the reader");
          fp_release (c);
          c->fp = FP_LOCKED;
        }
      else if (!c->locked && c->fp == FP_LOCKED)
        {
          c->fp = FP_BUSY;
          c->retry_claim_at = now;
        }
    }

  if (c->fp == FP_BUSY && now >= c->retry_claim_at)
    fp_arm (c);
  if (c->fp != FP_OFF && c->deadline && now >= c->deadline)
    {
      notify (c, "Fingerprint timed out; type your password");
      fp_off (c);
    }
}

static int
poll_timeout_ms (Ctx *c)
{
  uint64_t next = UINT64_MAX, bus_timeout = UINT64_MAX, now = now_usec ();

  if (c->bus && sd_bus_get_timeout (c->bus, &bus_timeout) >= 0)
    next = bus_timeout;
  if (c->fp == FP_BUSY && c->retry_claim_at < next)
    next = c->retry_claim_at;
  if (c->fp != FP_OFF && c->deadline && c->deadline < next)
    next = c->deadline;
  if (next == UINT64_MAX)
    return -1;
  if (next <= now)
    return 0;
  return (int) ((next - now + 999) / 1000 > 60000 ? 60000 : (next - now + 999) / 1000);
}

static Result
run (Ctx *c)
{
  for (;;)
    {
      struct pollfd fds[2];
      int nfds = 0, input = -1, r = 0;

      if (got_signal)
        return RES_SIGNAL;

      while (c->bus && (r = sd_bus_process (c->bus, NULL)) > 0)
        ;
      if (c->bus && r < 0)
        {
          /* Lost the system bus: keep accepting the password only. */
          c->claimed = c->verifying = false;
          c->fp = FP_OFF;
          c->verify_slot = sd_bus_slot_unref (c->verify_slot);
          c->lock_slot = sd_bus_slot_unref (c->lock_slot);
          c->bus = sd_bus_flush_close_unref (c->bus);
        }
      if (c->matched)
        return RES_MATCH;
      if (c->bus)
        apply_events (c);

      if (c->bus)
        {
          fds[nfds].fd = sd_bus_get_fd (c->bus);
          fds[nfds].events = sd_bus_get_events (c->bus);
          fds[nfds++].revents = 0;
        }
      input = nfds;
      fds[nfds].fd = c->mode == MODE_TTY ? c->tty : c->notify_read;
      fds[nfds].events = POLLIN;
      fds[nfds++].revents = 0;

      r = poll (fds, nfds, poll_timeout_ms (c));
      if (got_signal)
        return RES_SIGNAL;
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          return RES_CONV_ERROR;
        }
      if (fds[input].revents & (POLLHUP | POLLERR | POLLNVAL) && !(fds[input].revents & POLLIN))
        return RES_CONV_ERROR;
      if (fds[input].revents & POLLIN)
        {
          if (c->mode == MODE_TTY)
            {
              if (tty_input (c))
                return RES_PASSWORD;
            }
          else
            {
              uint64_t count;
              if (read (c->notify_read, &count, sizeof (count)) == sizeof (count))
                {
                  atomic_thread_fence (memory_order_acquire);
                  c->conv_done = true;
                  return c->shared->status == PAM_SUCCESS ? RES_PASSWORD : RES_CONV_ERROR;
                }
            }
        }
    }
}

/* ---- PAM entry points ---- */

static bool
is_remote (pam_handle_t *pamh)
{
  const void *rhost = NULL;
  char *session = NULL;
  bool remote = false;

  if (pam_get_item (pamh, PAM_RHOST, &rhost) == PAM_SUCCESS && rhost &&
      *(const char *) rhost && strcmp (rhost, "localhost") != 0)
    return true;
  /* Only ever disables the fingerprint, so the caller's environment is fine. */
  if (getenv ("SSH_CONNECTION") || getenv ("SSH_CLIENT") || getenv ("SSH_TTY"))
    return true;
  if (sd_pid_get_session (0, &session) >= 0)
    remote = sd_session_is_remote (session) > 0;
  free (session);
  return remote;
}

static void
parse_args (Ctx *c, int argc, const char **argv)
{
  for (int i = 0; i < argc; i++)
    {
      const char *arg = argv[i];
      if (strcmp (arg, "mode=tty") == 0)
        c->mode = MODE_TTY;
      else if (strcmp (arg, "mode=conv") == 0)
        c->mode = MODE_CONV;
      else if (strncmp (arg, "timeout=", 8) == 0)
        c->timeout_s = (unsigned) strtoul (arg + 8, NULL, 10);
      else if (strncmp (arg, "max-tries=", 10) == 0)
        c->max_tries = (unsigned) strtoul (arg + 10, NULL, 10);
      else if (strncmp (arg, "lock-session=", 13) == 0)
        c->lock_session = arg + 13;
      else if (strncmp (arg, "bus=", 4) == 0)
        c->bus_address = arg + 4;
      else if (strcmp (arg, "debug") == 0)
        c->debug = true;
      else
        pam_syslog (c->pamh, LOG_WARNING, "unknown option %s", arg);
    }
  if (c->max_tries == 0)
    c->max_tries = 1;
}

static void
cleanup (Ctx *c)
{
  fp_release (c);
  sd_bus_slot_unref (c->verify_slot);
  sd_bus_slot_unref (c->lock_slot);
  sd_bus_flush_close_unref (c->bus);
  free (c->device);
  free (c->fprintd_owner);
  free (c->logind_owner);
  if (c->tty >= 0)
    {
      tty_end (c);
      close (c->tty);
    }
  if (c->shared)
    shared_unref (c->shared);
  explicit_bzero (c->password, sizeof (c->password));
}

EXPORT int
pam_sm_authenticate (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  Ctx c = {
    .pamh = pamh,
    .mode = MODE_TTY,
    .max_tries = 3,
    .bus_address = SYSTEM_BUS,
    .lock_session = "auto",
    .silent = flags & PAM_SILENT,
    .tty = -1,
    .notify_read = -1,
  };
  const void *service = NULL;
  Result result;
  int status = PAM_IGNORE;
  int signo = 0;

  parse_args (&c, argc, argv);
  /* A token left from an earlier round would make pam_unix skip its prompt
   * when this module steps aside. */
  pam_set_item (pamh, PAM_AUTHTOK, NULL);

  if (pam_get_user (pamh, &c.user, NULL) != PAM_SUCCESS || !c.user || !*c.user)
    return PAM_IGNORE;
  if (is_remote (pamh))
    {
      debug_log (&c, "remote session; leaving %s to the password prompt", c.user);
      return PAM_IGNORE;
    }
  pam_get_item (pamh, PAM_SERVICE, &service);
  c.service = service ? service : "login";

  if (c.mode == MODE_TTY)
    {
      /* Piped input (sudo -S, pipelines) is not ours to take over. */
      if (!isatty (STDIN_FILENO))
        return PAM_IGNORE;
      c.tty = open ("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
      if (c.tty < 0)
        return PAM_IGNORE;
      snprintf (c.prompt, sizeof (c.prompt), "[%s] password or fingerprint for %s: ", c.service, c.user);
    }
  else
    {
      snprintf (c.prompt, sizeof (c.prompt), "Password or fingerprint: ");
    }

  if (!fp_probe (&c))
    goto out;
  if (c.locked)
    c.fp = FP_LOCKED;
  else
    fp_arm (&c);
  if (c.fp == FP_OFF)
    goto out;
  if (c.timeout_s)
    c.deadline = now_usec () + c.timeout_s * USEC_PER_SEC;

  /* Dialogs (KDE) show this instead of the prompt text. */
  if (c.mode == MODE_CONV)
    notify (&c, c.swipe ?
            "Swipe your finger or type your password" :
            "Touch the fingerprint reader or type your password");
  if (c.mode == MODE_TTY ? !tty_begin (&c) : !conv_begin (&c))
    goto out;

  result = run (&c);
  switch (result)
    {
    case RES_MATCH:
      pam_syslog (pamh, LOG_INFO, "fingerprint accepted for %s", c.user);
      status = PAM_SUCCESS;
      break;

    case RES_PASSWORD:
      /* Hand the password to pam_unix (try_first_pass) and step aside. */
      c.password[c.password_len] = '\0';
      if (pam_set_item (pamh, PAM_AUTHTOK,
                        c.mode == MODE_TTY ? c.password : c.shared->answer) == PAM_SUCCESS)
        status = PAM_IGNORE;
      else
        status = PAM_SYSTEM_ERR;
      break;

    case RES_SIGNAL:
      signo = got_signal;
      status = PAM_CONV_ERR;
      break;

    case RES_CONV_ERROR:
      /* shared->status is published by the eventfd handshake; on any other
       * error path the thread may still be running. */
      status = c.conv_done ? c.shared->status : PAM_CONV_ERR;
      if (status == PAM_SUCCESS)
        status = PAM_CONV_ERR;
      break;
    }

out:
  cleanup (&c);
  /* The terminal and the original handlers are back: deliver Ctrl+C as if
   * it had been pressed at the application's own prompt. */
  if (signo)
    raise (signo);
  return status;
}

EXPORT int
pam_sm_setcred (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  return PAM_IGNORE;
}
