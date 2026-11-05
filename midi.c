/* MIDI's sequencer (midi.h): ALSA's events, on a thread of their own,
 * into the state in midi_state.c. */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "midi.h"

typedef void (*aither_message_fn)(const unsigned char* m, int len);   /* alsa_midi.c */
int  aither_alsa_open(const char* client_name);
int  aither_alsa_connect_from(int client, int port);
int  aither_alsa_list_ports(char* buf, int bufsize);
int  aither_alsa_auto_connect(char* outbuf, int outsize);
int  aither_alsa_parse_address(const char* spec, int* client, int* port);
void aither_alsa_run(aither_message_fn on_message);
void aither_alsa_close(void);

/* ------------------------------------------------------------ the sequencer */
/* The thread blocks in alsa_midi.c's event loop and calls the writer. The
 * last subscription is remembered, so a loop that exits unexpectedly
 * (BUGS_AND_ISSUES.md 4c: a keyboard that silently stops after a few
 * reloads while `midi list` still shows it) can be resubscribed. */

static pthread_t thread;
static int thread_started;
static atomic_int thread_active;    /* true before the loop runs, false once it ends */
static atomic_int shutting_down;    /* an intended close: no "dropped" line */
static char last_info[512];         /* who, for `list` */
static int last_client = -1, last_port = -1;

static void* run_loop(void* arg) {
  (void)arg;
  aither_alsa_run(midi_message);
  thread_active = 0;
  if (!shutting_down)
    fprintf(stderr, "[aither] MIDI subscription dropped — next `aither send` will attempt auto-resubscribe\n");
  return NULL;
}

int midi_open(const char* client_name) { return aither_alsa_open(client_name) == 0; }

char* midi_list_ports(void) {
  char buf[4096] = "";
  return strdup(aither_alsa_list_ports(buf, sizeof buf) > 0 ? buf : "");
}

char* midi_auto_connect(void) {
  char buf[256] = "";
  if (aither_alsa_auto_connect(buf, sizeof buf) != 0) return strdup("");
  snprintf(last_info, sizeof last_info, "%s", buf);
  /* The "name (client:port)" trailer, so a resubscribe needn't search again. */
  char* lp = strrchr(buf, '(');
  char* rp = strrchr(buf, ')');
  if (lp && lp > buf && rp > lp + 1) {
    char* colon = memchr(lp + 1, ':', rp - lp - 1);
    if (colon && colon > lp + 1) {
      char *e1, *e2;
      long c = strtol(lp + 1, &e1, 10), p = strtol(colon + 1, &e2, 10);
      if (e1 == colon && e2 == rp) {
        last_client = (int)c;
        last_port = (int)p;
      }
    }
  }
  return strdup(buf);
}

int midi_connect(const char* spec) {
  int client, port;
  if (aither_alsa_parse_address(spec, &client, &port) != 0) return 0;
  if (aither_alsa_connect_from(client, port) != 0) return 0;
  last_client = client;
  last_port = port;
  snprintf(last_info, sizeof last_info, "%s (%d:%d)", spec, client, port);
  return 1;
}

void midi_start_thread(void) {
  if (thread_started) return;
  shutting_down = 0;
  thread_active = 1;
  pthread_create(&thread, NULL, run_loop, NULL);
  thread_started = 1;
}

void midi_shutdown(void) {
  /* Close the sequencer, which unblocks the thread, and join it. */
  if (!thread_started) return;
  shutting_down = 1;
  aither_alsa_close();
  pthread_join(thread, NULL);
  thread_started = 0;
}

int midi_subscription_active(void) { return thread_active; }
const char* midi_last_connect_info(void) { return last_info; }

char* midi_resubscribe_if_dropped(void) {
  if (thread_active || last_client < 0) return NULL;
  midi_shutdown();                   /* the thread is gone; the sequencer may be open */
  char* r;
  if (!midi_open("aither")) {
    r = strdup("fail: ALSA seq open");
  } else if (aither_alsa_connect_from(last_client, last_port) != 0) {
    if (asprintf(&r, "fail: subscribe to %d:%d", last_client, last_port) < 0) r = NULL;
  } else {
    midi_start_thread();
    if (asprintf(&r, "ok: resubscribed to %s", last_info) < 0) r = NULL;
  }
  return r;
}
