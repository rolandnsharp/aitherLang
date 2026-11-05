/* Thin C wrapper around ALSA seq — isolates aither from the header's
 * unions and bitfields. midi.c needs only the entry points and three
 * callback function pointers. */

#include <alsa/asoundlib.h>
#include <string.h>
#include <stdio.h>

typedef void (*aither_message_fn)(const unsigned char* m, int len);

static snd_seq_t* g_seq = NULL;
static int g_my_port = -1;
static volatile int g_running = 0;

int aither_alsa_open(const char* client_name) {
  if (g_seq) return 0;
  if (snd_seq_open(&g_seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
    g_seq = NULL;
    return -1;
  }
  snd_seq_set_client_name(g_seq, client_name ? client_name : "aither");
  g_my_port = snd_seq_create_simple_port(
      g_seq, "in",
      SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_APPLICATION);
  if (g_my_port < 0) {
    snd_seq_close(g_seq);
    g_seq = NULL;
    return -2;
  }
  return 0;
}

int aither_alsa_connect_from(int client, int port) {
  if (!g_seq || g_my_port < 0) return -1;
  snd_seq_addr_t sender; sender.client = client; sender.port = port;
  snd_seq_addr_t dest;   dest.client = snd_seq_client_id(g_seq); dest.port = g_my_port;
  snd_seq_port_subscribe_t* sub;
  snd_seq_port_subscribe_alloca(&sub);
  snd_seq_port_subscribe_set_sender(sub, &sender);
  snd_seq_port_subscribe_set_dest(sub, &dest);
  return snd_seq_subscribe_port(g_seq, sub);
}

/* Enumerate all readable ports. Writes "client:port\tclient_name - port_name\n"
 * lines into buf. Returns number of ports listed. */
int aither_alsa_list_ports(char* buf, int bufsize) {
  if (!g_seq || bufsize <= 0) return 0;
  buf[0] = '\0';
  int count = 0, off = 0;
  snd_seq_client_info_t* cinfo;
  snd_seq_port_info_t* pinfo;
  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);
  snd_seq_client_info_set_client(cinfo, -1);
  while (snd_seq_query_next_client(g_seq, cinfo) >= 0) {
    int client = snd_seq_client_info_get_client(cinfo);
    if (client == snd_seq_client_id(g_seq)) continue;
    snd_seq_port_info_set_client(pinfo, client);
    snd_seq_port_info_set_port(pinfo, -1);
    while (snd_seq_query_next_port(g_seq, pinfo) >= 0) {
      unsigned int caps = snd_seq_port_info_get_capability(pinfo);
      if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
      if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;
      int port = snd_seq_port_info_get_port(pinfo);
      const char* pname = snd_seq_port_info_get_name(pinfo);
      const char* cname = snd_seq_client_info_get_name(cinfo);
      int n = snprintf(buf + off, bufsize - off, "%d:%d\t%s - %s\n",
                       client, port, cname ? cname : "?", pname ? pname : "?");
      if (n < 0 || n >= bufsize - off) return count;
      off += n;
      count++;
    }
  }
  return count;
}

/* Find the first connectable input port and subscribe to it. Skips
 * ALSA's own system client (0) and the through/timer client (14).
 * Writes a short "name (client:port)" identifier into outbuf for the
 * caller to log. Returns 0 on success, -1 if nothing found. */
int aither_alsa_auto_connect(char* outbuf, int outsize) {
  if (!g_seq) return -1;
  if (outsize > 0) outbuf[0] = '\0';
  snd_seq_client_info_t* cinfo;
  snd_seq_port_info_t* pinfo;
  snd_seq_client_info_alloca(&cinfo);
  snd_seq_port_info_alloca(&pinfo);
  snd_seq_client_info_set_client(cinfo, -1);
  while (snd_seq_query_next_client(g_seq, cinfo) >= 0) {
    int client = snd_seq_client_info_get_client(cinfo);
    if (client == snd_seq_client_id(g_seq)) continue;
    if (client == 0 || client == 14) continue;   /* system/through */
    snd_seq_port_info_set_client(pinfo, client);
    snd_seq_port_info_set_port(pinfo, -1);
    while (snd_seq_query_next_port(g_seq, pinfo) >= 0) {
      unsigned int caps = snd_seq_port_info_get_capability(pinfo);
      if (!(caps & SND_SEQ_PORT_CAP_READ)) continue;
      if (!(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;
      int port = snd_seq_port_info_get_port(pinfo);
      if (aither_alsa_connect_from(client, port) == 0) {
        const char* cname = snd_seq_client_info_get_name(cinfo);
        const char* pname = snd_seq_port_info_get_name(pinfo);
        snprintf(outbuf, outsize, "%s - %s (%d:%d)",
                 cname ? cname : "?", pname ? pname : "?", client, port);
        return 0;
      }
    }
  }
  return -1;
}

int aither_alsa_parse_address(const char* spec, int* client, int* port) {
  if (!g_seq) return -1;
  snd_seq_addr_t addr;
  if (snd_seq_parse_address(g_seq, &addr, spec) < 0) return -1;
  *client = addr.client;
  *port = addr.port;
  return 0;
}

/* Blocking event loop; caller runs this on a dedicated thread. Returns
 * when snd_seq_event_input fails (we closed the sequencer). */
void aither_alsa_run(aither_message_fn on_message) {
  /* Each event as the MIDI 1.0 message it stands for, channel and all,
   * into the one reader every source shares (midi_state.c,
   * midi_message): a host with raw MIDI calls the same. */
  if (!g_seq) return;
  g_running = 1;
  while (g_running) {
    snd_seq_event_t* ev = NULL;
    int err = snd_seq_event_input(g_seq, &ev);
    if (err < 0) {
      if (err == -EAGAIN) continue;
      break;    /* -ENOSPC and friends: bail; typically a close */
    }
    if (!ev) continue;
    unsigned char m[3];
    switch (ev->type) {
      case SND_SEQ_EVENT_NOTEON:      /* velocity 0 is a note-off, as MIDI has it; the reader knows */
        m[0] = 0x90 | (ev->data.note.channel & 15), m[1] = ev->data.note.note & 127, m[2] = ev->data.note.velocity & 127;
        on_message(m, 3);
        break;
      case SND_SEQ_EVENT_NOTEOFF:
        m[0] = 0x80 | (ev->data.note.channel & 15), m[1] = ev->data.note.note & 127, m[2] = 0;
        on_message(m, 3);
        break;
      case SND_SEQ_EVENT_CONTROLLER:
        m[0] = 0xB0 | (ev->data.control.channel & 15), m[1] = ev->data.control.param & 127,
        m[2] = ev->data.control.value & 127;
        on_message(m, 3);
        break;
      case SND_SEQ_EVENT_CONTROL14:
        /* ALSA's own 14-bit controller (0..31, 0..16383): the pair it
         * stands for, coarse then fine. */
        if (ev->data.control.param < 32) {
          m[0] = 0xB0 | (ev->data.control.channel & 15), m[1] = ev->data.control.param & 127,
          m[2] = ev->data.control.value >> 7 & 127;
          on_message(m, 3);
          m[1] = (ev->data.control.param + 32) & 127, m[2] = ev->data.control.value & 127;
          on_message(m, 3);
        }
        break;
      case SND_SEQ_EVENT_PITCHBEND: {  /* ALSA's -8192..8191, MIDI's 0..16383 */
        int v = ev->data.control.value + 8192;
        v = v < 0 ? 0 : v > 16383 ? 16383 : v;
        m[0] = 0xE0 | (ev->data.control.channel & 15), m[1] = v & 127, m[2] = v >> 7 & 127;
        on_message(m, 3);
        break;
      }
      case SND_SEQ_EVENT_CHANPRESS:
        m[0] = 0xD0 | (ev->data.control.channel & 15), m[1] = ev->data.control.value & 127;
        on_message(m, 2);
        break;
      default: break;
    }
  }
  g_running = 0;
}

void aither_alsa_close(void) {
  g_running = 0;
  if (g_seq) {
    snd_seq_close(g_seq);
    g_seq = NULL;
    g_my_port = -1;
  }
}
