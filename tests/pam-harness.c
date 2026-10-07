/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Minimal PAM application for tests/test-pam-parallel.py: authenticates one
 * user against a service file in a private configuration directory.
 *
 *   pam-harness CONFDIR SERVICE USER [--rhost HOST] [--answer TEXT] [--linger-ms MS]
 *
 * Prompts mentioning "fingerprint" (pam_fprint_parallel in conversation mode)
 * get TEXT, or a line read from stdin, which may block forever like polkit's
 * agent helper. Other prompts get "asked". --linger-ms keeps the process alive
 * after pam_end(), as polkit-agent-helper-1 does before exiting, so threads a
 * module left behind run against the unloaded module. */
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static const char *answer;

static int
conversation (int count, const struct pam_message **messages, struct pam_response **responses, void *data)
{
  struct pam_response *reply = calloc (count, sizeof (*reply));

  if (!reply)
    return PAM_BUF_ERR;
  for (int i = 0; i < count; i++)
    {
      const struct pam_message *message = messages[i];
      switch (message->msg_style)
        {
        case PAM_PROMPT_ECHO_OFF:
        case PAM_PROMPT_ECHO_ON:
          printf ("PROMPT:%s\n", message->msg);
          fflush (stdout);
          if (strstr (message->msg, "fingerprint"))
            {
              char line[256];
              if (answer)
                {
                  reply[i].resp = strdup (answer);
                }
              else if (fgets (line, sizeof (line), stdin))
                {
                  line[strcspn (line, "\n")] = '\0';
                  reply[i].resp = strdup (line);
                }
              else
                {
                  free (reply);
                  return PAM_CONV_ERR;
                }
            }
          else
            {
              reply[i].resp = strdup ("asked");
            }
          break;
        default:
          printf ("INFO:%s\n", message->msg);
          fflush (stdout);
          break;
        }
    }
  *responses = reply;
  return PAM_SUCCESS;
}

int
main (int argc, char **argv)
{
  struct pam_conv conv = { conversation, NULL };
  pam_handle_t *pamh = NULL;
  const char *rhost = NULL;
  long linger_ms = 0;
  struct termios term;
  int result;

  if (argc < 4)
    {
      fprintf (stderr, "usage: %s CONFDIR SERVICE USER [--rhost HOST] [--answer TEXT] [--linger-ms MS]\n", argv[0]);
      return 2;
    }
  for (int i = 4; i + 1 < argc; i += 2)
    {
      if (strcmp (argv[i], "--rhost") == 0)
        rhost = argv[i + 1];
      else if (strcmp (argv[i], "--answer") == 0)
        answer = argv[i + 1];
      else if (strcmp (argv[i], "--linger-ms") == 0)
        linger_ms = strtol (argv[i + 1], NULL, 10);
    }
  result = pam_start_confdir (argv[2], argv[3], &conv, argv[1], &pamh);
  if (result != PAM_SUCCESS)
    {
      fprintf (stderr, "pam_start_confdir: %d\n", result);
      return 2;
    }
  if (rhost)
    pam_set_item (pamh, PAM_RHOST, rhost);
  result = pam_authenticate (pamh, 0);
  printf ("RESULT:%s\n", pam_strerror (pamh, result));
  if (tcgetattr (STDIN_FILENO, &term) == 0)
    printf ("TERMIOS:echo=%d,icanon=%d\n", !!(term.c_lflag & ECHO), !!(term.c_lflag & ICANON));
  fflush (stdout);
  pam_end (pamh, result);
  if (linger_ms > 0)
    usleep (linger_ms * 1000);
  return 0;
}
