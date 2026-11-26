/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mgu_renderer.c - UxPlay's renderer API (renderers/video_renderer.h, audio_renderer.h, mux_renderer.h)
 * for the iDrive 7 head unit, stage 1: no GStreamer, nothing is shown or played. It counts what the
 * iPhone sends and reports it on stdout, one line a second while a stream runs, each line
 * "MGU {json}", read by the service (native/hp-airplay, receiver mode):
 *
 *   {"ev":"client","model":"iPhone18,2","name":"..."}           the client that connected
 *   {"ev":"video","codec":"h264","w":..,"h":..,"sw":..,"sh":..} the stream's size (source and shown)
 *   {"ev":"start"} / {"ev":"stop"}                               the video pipeline
 *   {"ev":"audio","ct":..}                                       the audio stream's compression type
 *   {"ev":"stats","frames":..,"fps":..,"kbps":..,"idr":..,"sps":..,"audio":..}
 *
 *   {"ev":"decoder","ok":true,"path":"/dev/shm/...","w":1920,"h":720}  the GPU decoder and frame file opened
 *   {"ev":"decoder","ok":false,"error":"..."}                          they could not (stage 1 counting goes on)
 *   {"ev":"sound","ok":true,"device":"plug:smartx:...","ct":8} / {"ev":"sound","ok":false,"error":"..."}
 *   {"ev":"soundstop"}                                                 the iPhone's session ended (its last connection)
 *                                                                      stage 3: the sound's decoder and ALSA device
 *
 * Stage 2: every access unit also goes to the GPU decoder (video/src/mgu_video.cpp), which writes the pictures
 * to mgu-render's frame file; stats then carry "shown" (pictures written) and "decerr" (failed access units).
 *
 * Screen mirroring goes through a jitter buffer (renderer/mgu_jitter.c): the access units and sound packets are handed
 * to the decoder and the sound at an even pace, by the iPhone's timestamps, MGU_JITTER_MS (default 200, 0..1000) after
 * the fastest pictures arrive, the sound on the pictures' timeline; 0 hands each on as it arrives. The command
 * "jitter <ms>" on stdin changes it. A sound packet more than 60 ms late is dropped while newer ones wait (after a
 * packet the iPhone sent again), so the sound stays on time. Stats then carry "jms" (the delay), "vq" / "aq" (access
 * units / sound packets waiting), "vlate" / "aqlate" (handed on more than 40 ms late), "aqdrop" (late sound dropped),
 * "aadj" (samples added or left out to follow the car's sound clock) and "agaps" (every gap in the sound: late sound
 * dropped, the device run dry, sound that did not fit).
 *
 * The film's rect within the picture, its own black bars left out (video/src/film_bars.cpp), when it changes:
 * {"ev":"film","x":..,"y":..,"w":..,"h":..} in the frame file's pixels, w 0 when there are no bars; the app shows the
 * film over the whole screen by it.
 *
 * The HUD (renderer/mgu_hud.c): "hud on [SIZE X Y PACKED]" on stdin, again at least every 3 s, puts the picture on
 * the head-up display through cluster-render, SIZE % of it placed at X %, Y % of the room left; "hud off", or no
 * "hud on" for 3 s, takes it off. Its events: hud.
 *
 * Stage 4: a video the iPhone hands over (AirPlay's video button: YouTube's HLS, served by UxPlay on localhost, or
 * a direct address) is played by player/mgu_hls.cpp in place of GStreamer's playbin: UxPlay starts it with the
 * address (video_renderer_init's uri, then video_renderer_start), the iPhone's pause / seek / progress bar come
 * through video_renderer_pause / _resume / _seek and video_get_playback_info. Its events: hls, hlsopen, hlspos,
 * hlsseek, hlsend, and start / stop / decoder / sound / soundstop / stats as above.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <glib.h>
#include "../lib/logger.h"
#include "../renderers/video_renderer.h"
#include "../renderers/audio_renderer.h"
#include "../renderers/mux_renderer.h"
#include "mgu_video.h"
#include "mgu_audio.h"
#include "mgu_hls.h"
#include "mgu_jitter.h"
#include "mgu_hud.h"
#include <unistd.h>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>

/* Started as root by the service (it cannot give a child supplementary groups), the receiver becomes MGU_RUN_AS
 * (nobody) with the groups in MGU_GROUPS (video: the GPU's render node; mgu_alsa: the sound devices) before
 * anything else runs; one that cannot drop root does not run at all. */
__attribute__((constructor)) static void mgu_drop_root(void) {
    const char *who = getenv("MGU_RUN_AS");
    if (!who || getuid() != 0) return;
    struct passwd *pw = getpwnam(who);
    uid_t uid = pw ? pw->pw_uid : 65534;
    gid_t gid = pw ? pw->pw_gid : 65534, gs[16];
    int n = 0;
    char list[256];
    snprintf(list, sizeof list, "%s", getenv("MGU_GROUPS") ? getenv("MGU_GROUPS") : "");
    for (char *save = NULL, *t = strtok_r(list, ",", &save); t && n < 16; t = strtok_r(NULL, ",", &save)) {
        struct group *g = getgrnam(t);
        if (g) gs[n++] = g->gr_gid;
    }
    if (setgroups((size_t) n, gs) != 0 || setgid(gid) != 0 || setuid(uid) != 0 || getuid() == 0 || geteuid() == 0) {
        fprintf(stderr, "mgu: cannot run as %s: refusing to run as root\n", who);
        _exit(126);
    }
    fprintf(stderr, "mgu: running as %s (uid %u) with %d groups\n", who, (unsigned) uid, n);
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint64_t frames, bytes, idr, sps, audio_packets, audio_bytes;
    uint64_t win_frames, win_bytes;
    unsigned long long shown, decerr;
    double win_start;
    bool running;
} st;

/* the jitter buffer's queues: access units for the GPU decoder, sound packets for the sound */
static mgu_jitter_t *vq, *aq;
static void present_video(const unsigned char *data, int len, int64_t due_ns, mgu_jitter_t *q, unsigned gen);
static void play_audio(const unsigned char *data, int len, int64_t due_ns, mgu_jitter_t *q, unsigned gen);
static pthread_once_t queues_once = PTHREAD_ONCE_INIT;
static volatile unsigned long long a_dropped;   /* late sound packets dropped (play_audio) */
static void make_queues(void) {
    const char *e = getenv("MGU_JITTER_MS");
    if (e && *e) mgu_jitter_set_delay(atoi(e));
    vq = mgu_jitter_new(present_video, false);   /* waits itself, after decoding: the picture is ready when due */
    aq = mgu_jitter_new(play_audio, true);
    mgu_jitter_anchor(aq, vq);
}
static void queues(void) { pthread_once(&queues_once, make_queues); }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* one line, whole (several threads emit) and flushed (stdout is a pipe to the service) */
static void emit(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flockfile(stdout);
    fputs("MGU ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    funlockfile(stdout);
    va_end(ap);
}

/* JSON string body: quotes, backslashes and control characters left out */
static void clean(char *out, size_t n, const char *in) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char) in[i];
        if (c < 0x20 || c == '"' || c == '\\') continue;
        out[j++] = (char) c;
    }
    out[j] = 0;
}

static void stats_locked(double t) {
    double dt = t - st.win_start;
    if (dt < 1.0) return;
    mgu_audio_stats_t a;
    mgu_audio_stats(&a);
    mgu_jitter_stats_t jv, ja;
    mgu_jitter_stats(vq, &jv);
    mgu_jitter_stats(aq, &ja);
    emit("{\"ev\":\"stats\",\"frames\":%llu,\"fps\":%.1f,\"kbps\":%.0f,\"idr\":%llu,\"sps\":%llu,\"audio\":%llu,\"shown\":%llu,\"decerr\":%llu,"
         "\"adec\":%llu,\"afull\":%llu,\"aerr\":%llu,\"axrun\":%llu,\"adev\":%.0f,"
         "\"jms\":%d,\"vq\":%u,\"vlate\":%llu,\"aq\":%u,\"aqlate\":%llu,\"aqdrop\":%llu,\"aadj\":%llu,\"agaps\":%llu}",
         (unsigned long long) st.frames, st.win_frames / dt, st.win_bytes * 8.0 / 1000.0 / dt,
         (unsigned long long) st.idr, (unsigned long long) st.sps, (unsigned long long) st.audio_packets, st.shown, st.decerr,
         a.decoded, a.dropped_full, a.errors, a.xruns, a.device_ms,
         jv.delay_ms, jv.queued, jv.late, ja.queued, ja.late, a_dropped, a.adjusted,
         a_dropped + a.xruns + a.dropped_full);
    st.win_frames = st.win_bytes = 0;
    st.win_start = t;
}

/* the player's events, as ours */
static void emit_json(const char *json) { emit("%s", json); }
static char *hls_uri = NULL;     /* the address of the video to play (stage 4), NULL for screen mirroring */

/* The service's commands on stdin, one a line: for a played video, the app's controls on the iDrive (the iPhone
 * follows them through /playback-info, the rate and the position, as it follows its own); for screen mirroring, the
 * jitter buffer's delay; the picture on the HUD.
 *   pause | resume | seek <seconds> | jitter <ms> | hud on [size x y packed [lead]] | hud off */
static void *mgu_commands(void *arg) {
    (void) arg;
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        double t = 0;
        int ms = 0;
        if (sscanf(line, "jitter %d", &ms) == 1) mgu_jitter_set_delay(ms);
        else if (!strncmp(line, "hud on", 6)) {    /* each second: not echoed */
            int sz, hx, hy, pk, ld;
            const int nf = sscanf(line, "hud on %d %d %d %d %d", &sz, &hx, &hy, &pk, &ld);
            if (nf >= 4) mgu_hud_layout(sz, hx, hy, pk);
            if (nf == 5) mgu_hud_lead(ld);
            mgu_hud_want(1);
            continue;
        }
        else if (!strncmp(line, "hud off", 7)) mgu_hud_want(0);
        else if (!strncmp(line, "pause", 5)) mgu_hls_pause(1);
        else if (!strncmp(line, "resume", 6)) mgu_hls_pause(0);
        else if (sscanf(line, "seek %lf", &t) == 1 && t >= 0) mgu_hls_seek(t);
        else continue;
        emit("{\"ev\":\"cmd\",\"line\":\"%.*s\"}", (int) strcspn(line, "\r\n\"\\"), line);
    }
    return NULL;
}
static float hls_start = 0.0f;

/* ---------------- video ---------------- */

bool gstreamer_init() { return true; }
bool gstreamer_decoder_check(const char *decoder) { (void) decoder; return true; }

void video_renderer_init(logger_t *logger, const char *server_name, videoflip_t videoflip[2], const char *parser, const char *rtp_pipeline,
                         const char *decoder, const char *converter, const char *videosink, const char *videosink_options,
                         bool initial_fullscreen, bool video_sync, bool h265_support, bool coverart_support,
                         guint playbin_version, const char *uri) {
    (void) logger; (void) server_name; (void) videoflip; (void) parser; (void) rtp_pipeline; (void) decoder; (void) converter;
    (void) videosink; (void) videosink_options; (void) initial_fullscreen; (void) video_sync; (void) h265_support;
    (void) coverart_support; (void) playbin_version;
    mgu_hls_set_emit(emit_json);
    free(hls_uri);
    hls_uri = uri ? strdup(uri) : NULL;
    queues();
    static bool once = false;
    if (!once) {
        once = true;
        atexit(mgu_video_close);   /* no frame file left behind */
        mgu_hud_init(emit_json);
        pthread_t t;
        if (pthread_create(&t, NULL, mgu_commands, NULL) == 0) pthread_detach(t);
    }
}

void video_renderer_start() {
    if (hls_uri) { mgu_hls_start(hls_uri, hls_start); hls_start = 0.0f; return; }   /* stage 4: the player says start */
    pthread_mutex_lock(&mu);
    st.running = true;
    st.win_start = now_s();
    st.win_frames = st.win_bytes = 0;
    pthread_mutex_unlock(&mu);
    emit("{\"ev\":\"start\"}");
}

void video_renderer_stop() {
    mgu_hls_stop();   /* a video playing (stage 4): ended, its sound and pictures with it */
    mgu_jitter_flush(vq);   /* nothing reaches the decoder after it stops */
    pthread_mutex_lock(&mu);
    bool was = st.running;
    st.running = false;
    pthread_mutex_unlock(&mu);
    if (was) emit("{\"ev\":\"stop\",\"frames\":%llu}", (unsigned long long) st.frames);
    mgu_video_stop();
}

void video_renderer_set_device_model(const char *model, const char *name) {
    char m[64], n[96];
    clean(m, sizeof m, model);
    clean(n, sizeof n, name);
    emit("{\"ev\":\"client\",\"model\":\"%s\",\"name\":\"%s\"}", m, n);
}

void video_renderer_set_track_metadata(const char *title, const char *artist, const char *album) { (void) title; (void) artist; (void) album; }
void video_renderer_pause() { mgu_hls_pause(1); }
/* the iPhone's /stop (on_video_stop) and a video's end (the EOS watch): the player stops, as GStreamer UxPlay's
 * pipeline goes to READY (in the car, 0.10.0: it played on after the phone's Stop, and the next video from the phone
 * ended after a few seconds) */
void video_renderer_hls_ready() { mgu_hls_stop(); }
void video_renderer_seek(float position) { mgu_hls_seek(position); }
void video_renderer_set_start(float position) { hls_start = position; }
void video_renderer_resume() { mgu_hls_pause(0); }
int video_renderer_cycle() { return 0; }
bool video_renderer_is_paused() { float r = 1; mgu_hls_info(NULL, NULL, NULL, NULL, &r, NULL, NULL); return r == 0.0f; }
bool video_renderer_eos_watch() { return mgu_hls_eos() != 0; }

/* the decoder holds each picture until it is due; 1 drops it (the queue was flushed) */
struct vwait { mgu_jitter_t *q; unsigned gen; };
static int vwait(long long pts_ms, void *arg) {
    const struct vwait *w = arg;
    if (pts_ms < 0) return 0;
    const int64_t due = (int64_t) pts_ms * 1000000LL;
    if (mgu_jitter_wait(w->q, due, w->gen)) return 1;
    if (mgu_jitter_now() > due + 40000000LL) mgu_jitter_late(w->q);
    return 0;
}

/* an access unit off the queue: decoded, its picture written to the frame file when due */
static void present_video(const unsigned char *data, int len, int64_t due_ns, mgu_jitter_t *q, unsigned gen) {
    struct vwait w = { q, gen };
    unsigned long long shown = 0, decerr = 0;
    int v = mgu_video_frame_timed(data, len, due_ns / 1000000LL, vwait, &w, &shown, &decerr);
    if (v == 1) {
        unsigned vw = 0, vh = 0;
        mgu_video_size(&vw, &vh);
        emit("{\"ev\":\"decoder\",\"ok\":true,\"path\":\"%s\",\"w\":%u,\"h\":%u}", mgu_video_path(), vw, vh);
    } else if (v == -1) {
        char e[200];
        clean(e, sizeof e, mgu_video_error());
        emit("{\"ev\":\"decoder\",\"ok\":false,\"error\":\"%s\"}", e);
    }
    pthread_mutex_lock(&mu);
    st.shown = shown; st.decerr = decerr;
    pthread_mutex_unlock(&mu);
    /* the film's rect when its black bars change (video/src/film_bars.cpp) */
    static unsigned film_seq = 0;
    unsigned fx, fy, fw, fh;
    const unsigned seq = mgu_video_film(&fx, &fy, &fw, &fh);
    if (seq != film_seq) {
        film_seq = seq;
        emit("{\"ev\":\"film\",\"x\":%u,\"y\":%u,\"w\":%u,\"h\":%u}", fx, fy, fw, fh);
    }
}

uint64_t video_renderer_render_buffer(unsigned char *data, int *data_len, int *nal_count, uint64_t *ntp_time) {
    (void) nal_count;
    int len = data_len ? *data_len : 0;
    bool idr = false, sps = false;
    /* Annex B: 00 00 00 01 <nal header> */
    for (int i = 0; i + 4 < len; i++) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            int type = data[i + 4] & 0x1f;
            if (type == 5) idr = true;
            if (type == 7) sps = true;
            i += 3;
        }
    }
    /* to the GPU decoder (stage 2) through the jitter buffer */
    queues();
    mgu_jitter_push(vq, data, len, ntp_time ? *ntp_time : 0);
    pthread_mutex_lock(&mu);
    st.frames++; st.win_frames++;
    st.bytes += len; st.win_bytes += len;
    if (idr) st.idr++;
    if (sps) st.sps++;
    stats_locked(now_s());
    pthread_mutex_unlock(&mu);
    return 0;
}

void video_renderer_display_jpeg(const void *data, int *data_len) { (void) data; (void) data_len; }
void video_renderer_flush() { mgu_jitter_flush(vq); }
unsigned int video_renderer_listen(void *loop, int id) { (void) loop; (void) id; return 0; }
void video_renderer_destroy() { video_renderer_stop(); mgu_video_close(); }
bool waiting_for_x11_window() { return false; }

/* As GStreamer UxPlay's: always "still playing" (true), the position -1 (not known: UxPlay answers nothing) until the
 * player has one. The end of a video goes through the EOS watch (video_renderer_eos_watch), never through here: the
 * iPhone asks for the progress right after /rate, before the main loop has started the player (in the car, 0.9.3:
 * answered "finished", UxPlay shut the video down before it began). */
bool video_get_playback_info(double *duration, double *position, double *seek_start, double *seek_duration, float *rate,
                             bool *buffer_empty, bool *buffer_full) {
    double d = 0, p = 0, ss = 0, sd = 0;
    float r = 0;
    int be = 1, bf = 0;
    const int on = mgu_hls_info(&d, &p, &ss, &sd, &r, &be, &bf);
    if (duration) *duration = on ? d : 0.0;
    if (position) *position = on && d > 0 ? p : -1.0;
    if (seek_start) *seek_start = on ? ss : 0.0;
    if (seek_duration) *seek_duration = on ? sd : 0.0;
    if (rate) *rate = on ? r : 0.0f;
    if (buffer_empty) *buffer_empty = be != 0;
    if (buffer_full) *buffer_full = bf != 0;
    return true;
}

int video_renderer_choose_codec_and_size(bool video_is_jpeg, bool video_is_h265, video_geometry_t *g) {
    if (video_is_jpeg) { emit("{\"ev\":\"video\",\"codec\":\"jpeg\"}"); return 0; }
    emit("{\"ev\":\"video\",\"codec\":\"%s\",\"w\":%d,\"h\":%d,\"sw\":%d,\"sh\":%d}", video_is_h265 ? "h265" : "h264",
         g ? g->width : 0, g ? g->height : 0, g ? g->width_source : 0, g ? g->height_source : 0);
    return 0;
}

void video_renderer_hls_set_volume(double volume) { mgu_hls_volume(volume); }
void video_renderer_set_window_handle(uintptr_t handle) { (void) handle; }

/* ---------------- audio ---------------- */

void audio_renderer_init(logger_t *logger, const char *audiosink, const bool *audio_sync, const bool *video_sync, const char *artp_pipeline) {
    (void) logger; (void) audiosink; (void) audio_sync; (void) video_sync; (void) artp_pipeline;
    queues();
}
/* a sound packet off the queue, when due: decoded and played; one more than 60 ms late is dropped while newer ones
 * wait, so the packets after a gap catch up with their time instead of overfilling the device */
static void play_audio(const unsigned char *data, int len, int64_t due_ns, mgu_jitter_t *q, unsigned gen) {
    (void) gen;
    if (mgu_jitter_now() > due_ns + 60000000LL && mgu_jitter_queued(q) > 0) { a_dropped++; return; }
    mgu_audio_packet(data, len);
}
static bool sound_on = false;   /* the sound started and not stopped yet */
void audio_renderer_start(unsigned char *compression_type) {
    const int ct = compression_type ? (int) *compression_type : -1;
    emit("{\"ev\":\"audio\",\"ct\":%d}", ct);
    char e[200], d[120];
    queues();
    mgu_jitter_flush(aq);
    if (mgu_audio_start(ct) == 0) {
        clean(d, sizeof d, mgu_audio_device());
        emit("{\"ev\":\"sound\",\"ok\":true,\"device\":\"%s\",\"ct\":%d}", d, ct);
        sound_on = true;
    } else {
        clean(e, sizeof e, mgu_audio_error());
        emit("{\"ev\":\"sound\",\"ok\":false,\"error\":\"%s\",\"ct\":%d}", e, ct);
    }
}
/* UxPlay calls this when the iPhone's last connection closes (conn_destroy): the session is over, whoever ended it */
void audio_renderer_stop() {
    mgu_jitter_flush(aq);
    mgu_audio_stop();
    if (sound_on) { sound_on = false; emit("{\"ev\":\"soundstop\"}"); }
}
void audio_renderer_render_buffer(unsigned char *data, int *data_len, unsigned short *seqnum, uint64_t *ntp_time) {
    (void) seqnum;
    queues();
    if (data && data_len) mgu_jitter_push(aq, data, *data_len, ntp_time ? *ntp_time : 0);   /* stage 3, through the jitter buffer */
    pthread_mutex_lock(&mu);
    st.audio_packets++;
    st.audio_bytes += data_len ? *data_len : 0;
    if (!st.running) stats_locked(now_s());   /* audio alone (no mirror video): still report */
    pthread_mutex_unlock(&mu);
}
void audio_renderer_set_volume(double volume) { mgu_audio_volume(volume); }
void audio_renderer_flush() { mgu_jitter_flush(aq); mgu_audio_flush(); }
void audio_renderer_destroy() { mgu_jitter_flush(aq); mgu_audio_stop(); }
unsigned int audio_renderer_listen(void *loop, int id) { (void) loop; (void) id; return 0; }

/* ---------------- recording (-mp4): not on the head unit ---------------- */

void mux_renderer_init(logger_t *logger, const char *filename, bool use_audio, bool use_video) {
    (void) logger; (void) filename; (void) use_audio; (void) use_video;
}
void mux_renderer_choose_audio_codec(unsigned char audio_ct) { (void) audio_ct; }
void mux_renderer_choose_video_codec(bool is_h265) { (void) is_h265; }
void mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time) { (void) data; (void) data_len; (void) ntp_time; }
void mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time) { (void) data; (void) data_len; (void) ntp_time; }
void mux_renderer_stop(void) {}
void mux_renderer_destroy(void) {}
