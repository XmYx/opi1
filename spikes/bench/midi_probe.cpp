// MIDI probe for the t3 spikes: connects to an ALSA sequencer client by name, sends note on/off pairs at
// a steady rate with a small random phase jitter, and logs the CLOCK_MONOTONIC time of every note-on.
//
//   midi_probe --to op1og-spike-cpp [--seconds 60] [--interval-ms 250] [--csv probe.csv]
//
// CSV: i,note,vel,t_send_ns. Note numbers cycle 48..83 and velocities 1..127, so (note, vel) identifies a
// send in the spike's own log. Other listeners (alsa-midi-record) can subscribe to op1og-probe:0.
#include <alsa/asoundlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <vector>

#include <unistd.h>

static int64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static void sleep_ns(int64_t ns) {
  timespec ts{time_t(ns / 1000000000), long(ns % 1000000000)};
  clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, nullptr);
}

int main(int argc, char** argv) {
  std::string to, csv;
  double seconds = 60, interval_ms = 250;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : "";
    if (a == "--to") to = v, ++i;
    else if (a == "--seconds") seconds = atof(v), ++i;
    else if (a == "--interval-ms") interval_ms = atof(v), ++i;
    else if (a == "--csv") csv = v, ++i;
    else { fprintf(stderr, "usage: midi_probe --to CLIENT [--seconds S] [--interval-ms MS] [--csv FILE]\n"); return 64; }
  }
  if (to.empty()) { fprintf(stderr, "midi_probe: --to is required\n"); return 64; }

  snd_seq_t* seq = nullptr;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { fprintf(stderr, "midi_probe: no sequencer\n"); return 1; }
  snd_seq_set_client_name(seq, "op1og-probe");
  int port = snd_seq_create_simple_port(seq, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
  // wait up to 10 s for the destination client to appear
  int dest = -1;
  for (int tries = 0; tries < 100 && dest < 0; ++tries) {
    snd_seq_client_info_t* ci;
    snd_seq_client_info_malloc(&ci);
    snd_seq_client_info_set_client(ci, -1);
    while (snd_seq_query_next_client(seq, ci) >= 0)
      if (to == snd_seq_client_info_get_name(ci)) dest = snd_seq_client_info_get_client(ci);
    snd_seq_client_info_free(ci);
    if (dest < 0) usleep(100000);
  }
  if (dest < 0 || snd_seq_connect_to(seq, port, dest, 0) < 0) {
    fprintf(stderr, "midi_probe: cannot reach %s\n", to.c_str());
    return 1;
  }
  fprintf(stderr, "midi_probe: %d:%d -> %s (%d:0)\n", snd_seq_client_id(seq), port, to.c_str(), dest);
  sleep_ns(1000000000);  // let the destination settle and other listeners subscribe

  struct Sent { int note, vel; int64_t t; };
  std::vector<Sent> sent;
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> jitter(0, 5000);  // us: spreads note arrivals over the audio period
  const int64_t end = now_ns() + int64_t(seconds * 1e9);
  const int64_t gate = 100000000, step = int64_t(interval_ms * 1e6);
  for (int i = 0; now_ns() < end; ++i) {
    int note = 48 + i % 36, vel = 1 + i % 127;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_source(&ev, port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_ev_set_noteon(&ev, 0, note, vel);
    const int64_t t = now_ns();
    snd_seq_event_output_direct(seq, &ev);
    sent.push_back({note, vel, t});
    sleep_ns(gate);
    snd_seq_ev_set_noteoff(&ev, 0, note, 0);
    snd_seq_event_output_direct(seq, &ev);
    sleep_ns(step - gate + int64_t(jitter(rng)) * 1000);
  }
  snd_seq_close(seq);
  if (!csv.empty()) {
    FILE* f = fopen(csv.c_str(), "w");
    if (!f) { perror(csv.c_str()); return 1; }
    fprintf(f, "i,note,vel,t_send_ns\n");
    for (size_t i = 0; i < sent.size(); ++i) fprintf(f, "%zu,%d,%d,%lld\n", i, sent[i].note, sent[i].vel, (long long)sent[i].t);
    fclose(f);
  }
  fprintf(stderr, "midi_probe: sent %zu notes\n", sent.size());
  return 0;
}
