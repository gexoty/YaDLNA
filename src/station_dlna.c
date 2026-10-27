/* Station DLNA, MIT. Audio-only UPnP AV renderer for Yandex Mini 2.
 * No shell execution, no config writes, one network loop; native GStreamer/ALSA.
 * See README for protocol limits and the separate Bluetooth integration. */
#define _GNU_SOURCE
#include "descriptions.h"
#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <dbus/dbus.h>
#include <dlfcn.h>
#include <errno.h>
#include <expat.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1.1"
#define AVT "urn:schemas-upnp-org:service:AVTransport:1"
#define RCS "urn:schemas-upnp-org:service:RenderingControl:1"
#define CMS "urn:schemas-upnp-org:service:ConnectionManager:1"
#define MP "org.mpris.MediaPlayer2.Player"
#define ROOT "org.mpris.MediaPlayer2"
#define PROPS "org.freedesktop.DBus.Properties"
#define OBJ "/org/mpris/MediaPlayer2"
#define CAP 98304
#define MAX_CLIENTS 16
#define MAX_SUBS 8
#define MAX_URI 4096
#define MAX_META 16384
#define HISTORY 16

typedef struct {
    char *s;
    size_t n, cap;
    int bad;
} Buf;
static void init(Buf *b) {
    memset(b, 0, sizeof(*b));
}
static void addn(Buf *b, const char *s, size_t n) {
    if (b->bad || b->n + n >= CAP) {
        b->bad = 1;
        return;
    }
    if (b->n + n + 1 > b->cap) {
        size_t c = (b->n + n + 1) * 2;
        if (c > CAP)
            c = CAP;
        char *p = realloc(b->s, c);
        if (!p) {
            b->bad = 1;
            return;
        }
        b->s = p;
        b->cap = c;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void add(Buf *b, const char *s) {
    addn(b, s, strlen(s));
}
static void fmt(Buf *b, const char *f, ...) {
    va_list a, c;
    va_start(a, f);
    va_copy(c, a);
    int n = vsnprintf(NULL, 0, f, c);
    va_end(c);
    if (n < 0 || n >= CAP) {
        b->bad = 1;
        va_end(a);
        return;
    }
    char *s = malloc((size_t)n + 1);
    if (!s) {
        b->bad = 1;
        va_end(a);
        return;
    }
    vsnprintf(s, (size_t)n + 1, f, a);
    va_end(a);
    addn(b, s, (size_t)n);
    free(s);
}
static void esc(Buf *b, const char *s) {
    for (; s && *s; s++)
        switch (*s) {
        case '&':
            add(b, "&amp;");
            break;
        case '<':
            add(b, "&lt;");
            break;
        case '>':
            add(b, "&gt;");
            break;
        case '"':
            add(b, "&quot;");
            break;
        case '\'':
            add(b, "&apos;");
            break;
        default:
            addn(b, s, 1);
        }
}
static void field(Buf *b, const char *k, const char *v) {
    fmt(b, "<%s>", k);
    esc(b, v);
    fmt(b, "</%s>", k);
}
static void clear(Buf *b) {
    free(b->s);
    init(b);
}
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static volatile sig_atomic_t running = 1;
static void sigstop(int n) {
    (void)n;
    running = 0;
}
static void logmsg(const char *s) {
    static unsigned count;
    if (count++ < 100)
        fprintf(stderr, "station-dlna: %s\n", s);
}

typedef struct {
    char *uri, *meta, *title;
} Track;
static Track current, next_track, history[HISTORY];
static unsigned nhistory = 0;
static char bind_ip[INET_ADDRSTRLEN], ifname[IFNAMSIZ] = "wlan0",
                                      friendly[96] = "Yandex Mini 2 DLNA", uuid[64];
static struct in_addr local_addr, netmask;
static int port = 49494, ssdpfd = -1, httpfd = -1, mock = 0, no_dbus = 0, mock_mixer = 0;
static int state = 0, transport_error = 0, changed = 1; /* stopped, playing, paused */
static double mock_started = 0, mock_pos = 0, volume = 1.0;
static int muted = 0;
static long mute_saved = -1;
static DBusConnection *dbus;

static void free_track(Track *t) {
    free(t->uri);
    free(t->meta);
    free(t->title);
    memset(t, 0, sizeof(*t));
}
static void copy_track(Track *to, const Track *from) {
    free_track(to);
    to->uri = strdup(from->uri ? from->uri : "");
    to->meta = strdup(from->meta ? from->meta : "");
    to->title = strdup(from->title ? from->title : "DLNA");
}
static const char *uri(void) {
    return current.uri ? current.uri : "";
}
static const char *meta(void) {
    return current.meta ? current.meta : "";
}
static const char *playback(void) {
    return state == 1 ? "Playing" : state == 2 ? "Paused" : "Stopped";
}
static const char *transport(void) {
    return !*uri()      ? "NO_MEDIA_PRESENT"
           : state == 3 ? "TRANSITIONING"
           : state == 1 ? "PLAYING"
           : state == 2 ? "PAUSED_PLAYBACK"
                        : "STOPPED";
}
static void remember(void) {
    if (!*uri())
        return;
    if (nhistory == HISTORY) {
        free_track(&history[0]);
        memmove(history, history + 1, (HISTORY - 1) * sizeof(Track));
        memset(&history[HISTORY - 1], 0, sizeof(Track));
        nhistory--;
    }
    copy_track(&history[nhistory++], &current);
}

/* Reject DTD/entities, bound depth and argument text; namespaces use Expat's '|'. */
typedef struct {
    XML_Parser p;
    char action[80], names[16][80];
    Buf vals[16];
    int depth, n, active, failed, body, action_seen;
} Soap;
static const char *localname(const char *s) {
    const char *p = strrchr(s, '|');
    return p ? p + 1 : s;
}
static void XMLCALL xml_start(void *v, const char *name, const char **attrs) {
    (void)attrs;
    Soap *x = v;
    x->depth++;
    const char *s = localname(name);
    if (x->depth > 12) {
        x->failed = 1;
        XML_StopParser(x->p, XML_FALSE);
        return;
    }
    if (x->depth == 1 && strcmp(s, "Envelope"))
        x->failed = 1;
    if (x->depth == 2 && !strcmp(s, "Body"))
        x->body = 1;
    if (x->depth == 3 && x->body) {
        if (x->action_seen || strlen(s) >= sizeof(x->action))
            x->failed = 1;
        else {
            strcpy(x->action, s);
            x->action_seen = 1;
        }
    }
    if (x->depth == 4 && x->body) {
        if (x->n == 16 || strlen(s) >= 80)
            x->failed = 1;
        else {
            for (int i = 0; i < x->n; i++)
                if (!strcmp(x->names[i], s))
                    x->failed = 1;
            strcpy(x->names[x->n], s);
            x->active = x->n++;
        }
    }
    if (x->depth > 4 && x->body)
        x->failed = 1;
}
static void XMLCALL xml_end(void *v, const char *name) {
    (void)name;
    Soap *x = v;
    if (x->depth == 4)
        x->active = -1;
    if (x->depth == 2)
        x->body = 0;
    x->depth--;
}
static void XMLCALL xml_text(void *v, const char *s, int n) {
    Soap *x = v;
    if (x->active >= 0) {
        addn(&x->vals[x->active], s, (size_t)n);
        if (x->vals[x->active].bad)
            x->failed = 1;
    }
}
static void XMLCALL xml_doctype(void *v, const char *a, const char *b, const char *c, int d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    Soap *x = v;
    x->failed = 1;
    XML_StopParser(x->p, XML_FALSE);
}
static int parse_soap(Soap *x, const char *s, size_t n) {
    memset(x, 0, sizeof(*x));
    x->active = -1;
    x->p = XML_ParserCreateNS(NULL, '|');
    if (!x->p)
        return 0;
    XML_SetUserData(x->p, x);
    XML_SetElementHandler(x->p, xml_start, xml_end);
    XML_SetCharacterDataHandler(x->p, xml_text);
    XML_SetStartDoctypeDeclHandler(x->p, xml_doctype);
    int ok = XML_Parse(x->p, s, (int)n, 1) == XML_STATUS_OK && !x->failed && x->action_seen;
    XML_ParserFree(x->p);
    return ok;
}
static const char *arg(Soap *x, const char *k) {
    for (int i = 0; i < x->n; i++)
        if (!strcmp(k, x->names[i]))
            return x->vals[i].s ? x->vals[i].s : "";
    return NULL;
}
static void free_soap(Soap *x) {
    for (int i = 0; i < x->n; i++)
        clear(&x->vals[i]);
}
static int valid_uri(const char *s) {
    if (!s || strlen(s) >= MAX_URI || (strncmp(s, "http://", 7) && strncmp(s, "https://", 8)))
        return 0;
    const char *host = strstr(s, "://") + 3;
    if (!*host || *host == '/' || *host == '?' || *host == '#')
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p <= 32 || *p == 127)
            return 0;
    return 1;
}
/* Extract title only, never treat DIDL metadata as a command or a file path. */
typedef struct {
    XML_Parser p;
    Buf text;
    int depth, in, fail;
} Title;
static void XMLCALL title_start(void *v, const char *n, const char **a) {
    (void)a;
    Title *t = v;
    if (++t->depth > 24) {
        t->fail = 1;
        XML_StopParser(t->p, XML_FALSE);
    }
    if (!strcmp(localname(n), "title"))
        t->in = t->depth;
}
static void XMLCALL title_end(void *v, const char *n) {
    (void)n;
    Title *t = v;
    if (t->in == t->depth)
        t->in = 0;
    t->depth--;
}
static void XMLCALL title_text(void *v, const char *s, int n) {
    Title *t = v;
    if (t->in && t->text.n + (size_t)n < 512)
        addn(&t->text, s, (size_t)n);
}
static void XMLCALL title_dtd(void *v, const char *a, const char *b, const char *c, int d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    Title *t = v;
    t->fail = 1;
    XML_StopParser(t->p, XML_FALSE);
}
static void set_track(Track *t, const char *u, const char *m) {
    free_track(t);
    t->uri = strdup(u);
    t->meta = strdup(m ? m : "");
    Title z = {0};
    z.p = XML_ParserCreateNS(NULL, '|');
    if (z.p && m && *m) {
        XML_SetUserData(z.p, &z);
        XML_SetElementHandler(z.p, title_start, title_end);
        XML_SetCharacterDataHandler(z.p, title_text);
        XML_SetStartDoctypeDeclHandler(z.p, title_dtd);
        if (XML_Parse(z.p, m, (int)strlen(m), 1) != XML_STATUS_OK)
            z.fail = 1;
    }
    if (z.p)
        XML_ParserFree(z.p);
    t->title = (!z.fail && z.text.n) ? strdup(z.text.s) : strdup("DLNA");
    clear(&z.text);
}

#include "https_transport.h"

/* Resolve only public GStreamer 1.x ABI already exported by the firmware. */
static struct {
    void *handle;
    GstElement *pipe;
    GstBus *bus;
    gboolean (*init_check)(int *, char ***, GError **);
    GstElement *(*parse_launch)(const gchar *, GError **);
    GstStateChangeReturn (*set_state)(GstElement *, GstState);
    gboolean (*query_position)(GstElement *, GstFormat, gint64 *);
    gboolean (*query_duration)(GstElement *, GstFormat, gint64 *);
    gboolean (*seek_simple)(GstElement *, GstFormat, GstSeekFlags, gint64);
    GstBus *(*get_bus)(GstPipeline *);
    void (*set_sync)(GstBus *, GstBusSyncHandler, gpointer, GDestroyNotify);
    void (*unref)(gpointer);
    void (*mini_unref)(GstMiniObject *);
    void (*error_free)(GError *);
    void (*gfree)(gpointer);
    void (*parse_error)(GstMessage *, GError **, gchar **);
    void (*parse_state)(GstMessage *, GstState *, GstState *, GstState *);
} gst;
static atomic_int gst_eos, gst_error, gst_playing, gst_error_code, gst_error_domain;
static GstBusSyncReply bus_sync(GstBus *b, GstMessage *m, gpointer v) {
    (void)b;
    (void)v;
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS)
        atomic_store(&gst_eos, 1);
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_STATE_CHANGED && (void *)m->src == (void *)gst.pipe) {
        GstState old, current, pending;
        gst.parse_state(m, &old, &current, &pending);
        if (current == GST_STATE_PLAYING)
            atomic_store(&gst_playing, 1);
    }
    if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gchar *d = NULL;
        gst.parse_error(m, &e, &d); /* Detailed errors can contain private stream URLs: opt in only
                                       on the local test bench. */
        if (e) {
            atomic_store(&gst_error_code, e->code);
            atomic_store(&gst_error_domain, e->domain);
        }
        if (e && getenv("STATION_DEBUG_AUDIO"))
            fprintf(stderr, "audio debug: %s\n", e->message);
        if (e)
            gst.error_free(e);
        if (d)
            gst.gfree(d);
        atomic_store(&gst_error, 1);
    }
    gst.mini_unref((GstMiniObject *)m);
    return GST_BUS_DROP;
}
static int audio_init(void) {
    if (mock)
        return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return 0;
    const char *path = getenv("STATION_GST_LIBRARY");
    if (!path)
        path = "/system/vendor/quasar/libs/libquasar_gstreamer.so";
    gst.handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!gst.handle) {
        fprintf(stderr, "audio library: %s\n", dlerror());
        return 0;
    }
#define SYM(member, name)                                                                          \
    do {                                                                                           \
        *(void **)(&gst.member) = dlsym(gst.handle, name);                                         \
        if (!gst.member) {                                                                         \
            fprintf(stderr, "missing symbol: %s\n", name);                                         \
            return 0;                                                                              \
        }                                                                                          \
    } while (0)
    SYM(init_check, "gst_init_check");
    SYM(parse_launch, "gst_parse_launch");
    SYM(set_state, "gst_element_set_state");
    SYM(query_position, "gst_element_query_position");
    SYM(query_duration, "gst_element_query_duration");
    SYM(seek_simple, "gst_element_seek_simple");
    SYM(get_bus, "gst_pipeline_get_bus");
    SYM(set_sync, "gst_bus_set_sync_handler");
    SYM(unref, "gst_object_unref");
    SYM(mini_unref, "gst_mini_object_unref");
    SYM(error_free, "g_error_free");
    SYM(gfree, "g_free");
    SYM(parse_error, "gst_message_parse_error");
    SYM(parse_state, "gst_message_parse_state_changed");
#undef SYM
    GError *e = NULL;
    if (!gst.init_check(NULL, NULL, &e)) {
        if (e)
            gst.error_free(e);
        return 0;
    }
    const char *plugins[] = {"coreelements",
                             "playback",
                             "typefindfunctions",
                             "audioconvert",
                             "audioresample",
                             "audioparsers",
                             "libav",
                             "flac",
                             "wavparse",
                             "ogg",
                             "opus",
                             "opusparse",
                             "soup",
                             "alsa_nocapturelog",
                             "isomp4",
                             "id3demux",
                             NULL};
    for (int i = 0; plugins[i]; i++) {
        char n[120];
        snprintf(n, sizeof(n), "gst_plugin_%s_register", plugins[i]);
        void (*reg)(void) = NULL;
        *(void **)(&reg) = dlsym(gst.handle, n);
        if (reg)
            reg();
    }
    return 1;
}
static void audio_stop(void) {
    media_proxy_stop();
    if (!mock && gst.pipe) {
        gst.set_state(gst.pipe, GST_STATE_NULL);
        gst.set_sync(gst.bus, NULL, NULL, NULL);
        gst.unref(gst.bus);
        gst.unref(gst.pipe);
        gst.pipe = NULL;
        gst.bus = NULL;
    }
    atomic_store(&gst_playing, 0);
    atomic_store(&gst_eos, 0);
    atomic_store(&gst_error, 0);
    mock_pos = 0;
    state = 0;
    changed = 1;
}
static void gst_quote(Buf *b, const char *s) {
    add(b, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            add(b, "\\");
        addn(b, s, 1);
    }
    add(b, "\"");
}
static int audio_play(void) {
    if (!*uri())
        return 0;
    if (state == 1 || state == 3)
        return 1;
    if (mock) {
        mock_started = now();
        state = 1;
        transport_error = 0;
        changed = 1;
        return 1;
    }
    if (!gst.pipe) {
        char local_url[100];
        if (!media_proxy_start(uri(), local_url))
            return 0;
        Buf p;
        init(&p);
        add(&p, "souphttpsrc location=");
        gst_quote(&p, local_url);
        add(&p, " timeout=10 ! decodebin ! audioconvert ! audioresample ! ");
        const char *sink = getenv("STATION_AUDIO_SINK");
        add(&p, sink ? sink : "alsasink device=default");
        GError *e = NULL;
        gst.pipe = gst.parse_launch(p.s, &e);
        clear(&p);
        if (e) {
            if (getenv("STATION_DEBUG_AUDIO"))
                fprintf(stderr, "pipeline debug: %s\n", e->message);
            gst.error_free(e);
            if (gst.pipe) {
                gst.unref(gst.pipe);
                gst.pipe = NULL;
            }
            return 0;
        }
        if (!gst.pipe)
            return 0;
        gst.bus = gst.get_bus((GstPipeline *)gst.pipe);
        if (!gst.bus) {
            gst.unref(gst.pipe);
            gst.pipe = NULL;
            return 0;
        }
        gst.set_sync(gst.bus, bus_sync, NULL, NULL);
    }
    if (gst.set_state(gst.pipe, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        audio_stop();
        return 0;
    }
    state = 3; /* Publish Playing only after pipeline confirms it. */
    transport_error = 0;
    changed = 1;
    return 1;
}
static int audio_pause(void) {
    if (state != 1 && state != 3)
        return state == 2;
    if (mock)
        mock_pos += now() - mock_started;
    else if (gst.set_state(gst.pipe, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE)
        return 0;
    state = 2;
    changed = 1;
    return 1;
}
static double position(void) {
    if (mock)
        return mock_pos + (state == 1 ? now() - mock_started : 0);
    gint64 n = 0;
    if (gst.pipe)
        gst.query_position(gst.pipe, GST_FORMAT_TIME, &n);
    return n > 0 ? n / 1e9 : 0;
}
static double duration(void) {
    if (mock)
        return *uri() ? 180 : 0;
    gint64 n = 0;
    if (gst.pipe)
        gst.query_duration(gst.pipe, GST_FORMAT_TIME, &n);
    return n > 0 ? n / 1e9 : 0;
}
static int seek_to(double s) {
    if (s < 0 || !isfinite(s))
        return 0;
    if (mock) {
        mock_pos = s;
        mock_started = now();
        return 1;
    }
    return gst.pipe &&
           gst.seek_simple(gst.pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                           (gint64)(s * 1e9));
}
static int select_next(int previous) {
    int old = state;
    if (previous) {
        if (!nhistory)
            return 0;
        audio_stop();
        copy_track(&next_track, &current);
        copy_track(&current, &history[--nhistory]);
        free_track(&history[nhistory]);
    } else {
        if (!next_track.uri || !*next_track.uri)
            return 0;
        remember();
        audio_stop();
        copy_track(&current, &next_track);
        free_track(&next_track);
    }
    transport_error = 0;
    changed = 1;
    if (old == 1 || old == 2 || old == 3) {
        if (!audio_play())
            return 0;
        if (old == 2)
            audio_pause();
    }
    return 1;
}

/* Use the station's existing softvol control, not a second player gain. */
static snd_ctl_t *ctl;
static snd_ctl_elem_id_t *ctl_id;
static long vol_min, vol_max;
static unsigned vol_count;
static const char *control_name = "Master Playback Volume";
static int mixer_read(long *v) {
    snd_ctl_elem_value_t *x;
    snd_ctl_elem_value_alloca(&x);
    snd_ctl_elem_value_set_id(x, ctl_id);
    if (snd_ctl_elem_read(ctl, x) < 0)
        return 0;
    *v = snd_ctl_elem_value_get_integer(x, 0);
    return 1;
}
static int mixer_write(long v) {
    snd_ctl_elem_value_t *x;
    snd_ctl_elem_value_alloca(&x);
    snd_ctl_elem_value_set_id(x, ctl_id);
    for (unsigned i = 0; i < vol_count; i++)
        snd_ctl_elem_value_set_integer(x, i, v);
    return snd_ctl_elem_write(ctl, x) >= 0;
}
static int mixer_init(void) {
    if (mock || mock_mixer)
        return 1;
    if (snd_ctl_open(&ctl, "hw:0", SND_CTL_NONBLOCK) < 0)
        return 0;
    snd_ctl_elem_id_malloc(&ctl_id);
    snd_ctl_elem_id_set_interface(ctl_id, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_id_set_name(ctl_id, control_name);
    snd_ctl_elem_info_t *i;
    snd_ctl_elem_info_alloca(&i);
    snd_ctl_elem_info_set_id(i, ctl_id);
    if (snd_ctl_elem_info(ctl, i) < 0 ||
        snd_ctl_elem_info_get_type(i) != SND_CTL_ELEM_TYPE_INTEGER ||
        !snd_ctl_elem_info_is_writable(i))
        return 0;
    vol_min = snd_ctl_elem_info_get_min(i);
    vol_max = snd_ctl_elem_info_get_max(i);
    vol_count = snd_ctl_elem_info_get_count(i);
    long v;
    if (vol_max <= vol_min || !vol_count || !mixer_read(&v))
        return 0;
    volume = (double)(v - vol_min) / (vol_max - vol_min);
    return 1;
}
static int set_volume(double v) {
    if (!isfinite(v) || v < 0 || v > 1)
        return 0;
    if (!mock && !mock_mixer) {
        long raw = vol_min + lround(v * (vol_max - vol_min));
        if (muted)
            mute_saved = raw;
        else if (!mixer_write(raw))
            return 0;
    }
    volume = v;
    changed = 1;
    return 1;
}
static int set_mute(int m) {
    if (m == muted)
        return 1;
    if (!mock && !mock_mixer) {
        if (m) {
            if (!mixer_read(&mute_saved) || !mixer_write(vol_min))
                return 0;
        } else if (!mixer_write(mute_saved >= vol_min ? mute_saved : vol_min))
            return 0;
    }
    muted = m;
    changed = 1;
    return 1;
}
static void mixer_poll(void) {
    if (mock || mock_mixer)
        return;
    long v;
    if (mixer_read(&v)) {
        if (muted && v != vol_min) {
            muted = 0;
            changed = 1;
        }
        if (!muted) {
            double f = (double)(v - vol_min) / (vol_max - vol_min);
            if (fabs(f - volume) > 0.0001) {
                volume = f;
                changed = 1;
            }
        }
    }
}

static void time_string(double n, char out[32]) {
    unsigned s = n > 0 && n < 360000000 ? n : 0;
    snprintf(out, 32, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
}
static int uint_arg(const char *s, unsigned max, unsigned *out) {
    if (!s || !*s)
        return 0;
    char *e;
    errno = 0;
    unsigned long n = strtoul(s, &e, 10);
    if (errno || *e || n > max || *s == '-')
        return 0;
    *out = (unsigned)n;
    return 1;
}
static void actions(char out[80]) {
    strcpy(out, *uri() ? "Play,Stop" : "");
    if (state == 1)
        strcat(out, ",Pause");
    if (next_track.uri && *next_track.uri)
        strcat(out, ",Next");
    if (nhistory)
        strcat(out, ",Previous");
    if (gst.pipe || mock)
        strcat(out, ",Seek");
}
static int soap_action(int service, Soap *x, Buf *b) {
    const char *a = x->action;
    unsigned i;
    if (service != 2 && (!uint_arg(arg(x, "InstanceID"), 0, &i)))
        return 718;
    if (service == 0) {
        if (!strcmp(a, "SetAVTransportURI") || !strcmp(a, "SetNextAVTransportURI")) {
            int next = !strcmp(a, "SetNextAVTransportURI");
            const char *u = arg(x, next ? "NextURI" : "CurrentURI"),
                       *m = arg(x, next ? "NextURIMetaData" : "CurrentURIMetaData");
            if (!u || !m || strlen(m) >= MAX_META || (*u && !valid_uri(u)))
                return 402;
            if (next) {
                set_track(&next_track, u, m);
                changed = 1;
                return 0;
            }
            audio_stop();
            remember();
            set_track(&current, u, m);
            free_track(&next_track);
            transport_error = 0;
            changed = 1;
            return 0;
        }
        if (!strcmp(a, "Play")) {
            const char *s = arg(x, "Speed");
            if (!s || strcmp(s, "1"))
                return 717;
            if (!*uri())
                return 702;
            if (!audio_play())
                return 704;
            return 0;
        }
        if (!strcmp(a, "Pause"))
            return audio_pause() ? 0 : 701;
        if (!strcmp(a, "Stop")) {
            audio_stop();
            transport_error = 0;
            return 0;
        }
        if (!strcmp(a, "Next") || !strcmp(a, "Previous"))
            return select_next(!strcmp(a, "Previous")) ? 0 : 711;
        if (!strcmp(a, "Seek")) {
            const char *u = arg(x, "Unit"), *t = arg(x, "Target");
            if (!u || strcmp(u, "REL_TIME"))
                return 710;
            unsigned h, m, s;
            char c;
            if (!t || sscanf(t, "%u:%u:%u%c", &h, &m, &s, &c) != 3 || h > 9999 || m > 59 || s > 59)
                return 402;
            return seek_to(h * 3600.0 + m * 60 + s) ? 0 : 710;
        }
        if (!strcmp(a, "GetTransportInfo")) {
            field(b, "CurrentTransportState", transport());
            field(b, "CurrentTransportStatus", transport_error ? "ERROR_OCCURRED" : "OK");
            field(b, "CurrentSpeed", "1");
            return 0;
        }
        if (!strcmp(a, "GetPositionInfo")) {
            char d[32], p[32];
            time_string(duration(), d);
            time_string(position(), p);
            field(b, "Track", *uri() ? "1" : "0");
            field(b, "TrackDuration", d);
            field(b, "TrackMetaData", meta());
            field(b, "TrackURI", uri());
            field(b, "RelTime", p);
            field(b, "AbsTime", p);
            field(b, "RelCount", "2147483647");
            field(b, "AbsCount", "2147483647");
            return 0;
        }
        if (!strcmp(a, "GetMediaInfo")) {
            char d[32];
            time_string(duration(), d);
            field(b, "NrTracks", *uri() ? "1" : "0");
            field(b, "MediaDuration", d);
            field(b, "CurrentURI", uri());
            field(b, "CurrentURIMetaData", meta());
            field(b, "NextURI", next_track.uri ? next_track.uri : "");
            field(b, "NextURIMetaData", next_track.meta ? next_track.meta : "");
            field(b, "PlayMedium", *uri() ? "NETWORK" : "NONE");
            field(b, "RecordMedium", "NOT_IMPLEMENTED");
            field(b, "WriteStatus", "NOT_IMPLEMENTED");
            return 0;
        }
        if (!strcmp(a, "GetDeviceCapabilities")) {
            field(b, "PlayMedia", "NETWORK");
            field(b, "RecMedia", "NOT_IMPLEMENTED");
            field(b, "RecQualityModes", "NOT_IMPLEMENTED");
            return 0;
        }
        if (!strcmp(a, "GetTransportSettings")) {
            field(b, "PlayMode", "NORMAL");
            field(b, "RecQualityMode", "NOT_IMPLEMENTED");
            return 0;
        }
        if (!strcmp(a, "GetCurrentTransportActions")) {
            char v[80];
            actions(v);
            field(b, "Actions", v);
            return 0;
        }
        if (!strcmp(a, "SetPlayMode")) {
            const char *v = arg(x, "NewPlayMode");
            return v && !strcmp(v, "NORMAL") ? 0 : 712;
        }
    } else if (service == 1) {
        if (!strcmp(a, "ListPresets")) {
            field(b, "CurrentPresetNameList", "FactoryDefaults");
            return 0;
        }
        const char *ch = arg(x, "Channel");
        if (!ch || strcmp(ch, "Master"))
            return 402;
        if (!strcmp(a, "GetVolume")) {
            fmt(b, "<CurrentVolume>%d</CurrentVolume>", (int)lround(volume * 100));
            return 0;
        }
        if (!strcmp(a, "SetVolume")) {
            if (!uint_arg(arg(x, "DesiredVolume"), 100, &i))
                return 402;
            return set_volume(i / 100.0) ? 0 : 501;
        }
        if (!strcmp(a, "GetMute")) {
            field(b, "CurrentMute", muted ? "1" : "0");
            return 0;
        }
        if (!strcmp(a, "SetMute")) {
            const char *v = arg(x, "DesiredMute");
            if (!v)
                return 402;
            if (!strcmp(v, "true"))
                i = 1;
            else if (!strcmp(v, "false"))
                i = 0;
            else if (!uint_arg(v, 1, &i))
                return 402;
            return set_mute((int)i) ? 0 : 501;
        }
    } else {
        if (!strcmp(a, "GetProtocolInfo")) {
            field(b, "Source", "");
            field(b, "Sink", SINK_PROTOCOLS);
            return 0;
        }
        if (!strcmp(a, "GetCurrentConnectionIDs")) {
            field(b, "ConnectionIDs", "0");
            return 0;
        }
        if (!strcmp(a, "GetCurrentConnectionInfo")) {
            if (!uint_arg(arg(x, "ConnectionID"), 0, &i))
                return 706;
            field(b, "RcsID", "0");
            field(b, "AVTransportID", "0");
            field(b, "ProtocolInfo", "http-get:*:audio/mpeg:*");
            field(b, "PeerConnectionManager", "");
            field(b, "PeerConnectionID", "-1");
            field(b, "Direction", "Input");
            field(b, "Status", "OK");
            return 0;
        }
    }
    return 401;
}

/* MPRIS on the private Bluetooth bus; no D-Bus activations or mutating calls out. */
static void variant(DBusMessageIter *i, int type, void *v, const char *sig) {
    DBusMessageIter a;
    dbus_message_iter_open_container(i, DBUS_TYPE_VARIANT, sig, &a);
    dbus_message_iter_append_basic(&a, type, v);
    dbus_message_iter_close_container(i, &a);
}
static void prop_basic(DBusMessageIter *i, const char *k, int t, void *v, const char *sig) {
    DBusMessageIter d;
    dbus_message_iter_open_container(i, DBUS_TYPE_DICT_ENTRY, NULL, &d);
    dbus_message_iter_append_basic(&d, DBUS_TYPE_STRING, &k);
    variant(&d, t, v, sig);
    dbus_message_iter_close_container(i, &d);
}
static void metadata(DBusMessageIter *i) {
    DBusMessageIter v, a;
    dbus_message_iter_open_container(i, DBUS_TYPE_VARIANT, "a{sv}", &v);
    dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "{sv}", &a);
    const char *t = current.title ? current.title : "DLNA", *u = uri(),
               *id = *u ? "/org/mpris/MediaPlayer2/track/current"
                        : "/org/mpris/MediaPlayer2/TrackList/NoTrack";
    prop_basic(&a, "xesam:title", DBUS_TYPE_STRING, &t, "s");
    prop_basic(&a, "xesam:url", DBUS_TYPE_STRING, &u, "s");
    prop_basic(&a, "mpris:trackid", DBUS_TYPE_OBJECT_PATH, &id, "o");
    dbus_int64_t len = duration() * 1000000;
    prop_basic(&a, "mpris:length", DBUS_TYPE_INT64, &len, "x");
    dbus_message_iter_close_container(&v, &a);
    dbus_message_iter_close_container(i, &v);
}
static void metadata_prop(DBusMessageIter *i) {
    DBusMessageIter d;
    const char *k = "Metadata";
    dbus_message_iter_open_container(i, DBUS_TYPE_DICT_ENTRY, NULL, &d);
    dbus_message_iter_append_basic(&d, DBUS_TYPE_STRING, &k);
    metadata(&d);
    dbus_message_iter_close_container(i, &d);
}
static int get_property(DBusMessageIter *i, const char *iface, const char *k) {
    dbus_bool_t b = FALSE;
    double d = 1;
    const char *s = "";
    dbus_int64_t x;
    if (!strcmp(iface, ROOT)) {
        if (!strcmp(k, "Identity")) {
            s = friendly;
            variant(i, DBUS_TYPE_STRING, &s, "s");
            return 1;
        }
        if (!strcmp(k, "CanQuit") || !strcmp(k, "CanRaise") || !strcmp(k, "HasTrackList")) {
            variant(i, DBUS_TYPE_BOOLEAN, &b, "b");
            return 1;
        }
        if (!strcmp(k, "SupportedUriSchemes") || !strcmp(k, "SupportedMimeTypes")) {
            DBusMessageIter v, a;
            dbus_message_iter_open_container(i, DBUS_TYPE_VARIANT, "as", &v);
            dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &a);
            const char *vals[] = {"http", "https", NULL};
            const char *types[] = {"audio/mpeg", "audio/flac", "audio/ogg",
                                   "audio/wav",  "audio/mp4",  NULL};
            const char **p = !strcmp(k, "SupportedUriSchemes") ? vals : types;
            for (; *p; p++)
                dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, p);
            dbus_message_iter_close_container(&v, &a);
            dbus_message_iter_close_container(i, &v);
            return 1;
        }
    }
    if (strcmp(iface, MP))
        return 0;
    if (!strcmp(k, "PlaybackStatus")) {
        s = playback();
        variant(i, DBUS_TYPE_STRING, &s, "s");
    } else if (!strcmp(k, "Metadata"))
        metadata(i);
    else if (!strcmp(k, "Volume")) {
        d = volume;
        variant(i, DBUS_TYPE_DOUBLE, &d, "d");
    } else if (!strcmp(k, "Position")) {
        x = position() * 1000000;
        variant(i, DBUS_TYPE_INT64, &x, "x");
    } else if (!strcmp(k, "Rate") || !strcmp(k, "MinimumRate") || !strcmp(k, "MaximumRate"))
        variant(i, DBUS_TYPE_DOUBLE, &d, "d");
    else if (!strcmp(k, "CanGoNext")) {
        b = next_track.uri && *next_track.uri;
        variant(i, DBUS_TYPE_BOOLEAN, &b, "b");
    } else if (!strcmp(k, "CanGoPrevious")) {
        b = nhistory > 0;
        variant(i, DBUS_TYPE_BOOLEAN, &b, "b");
    } else if (!strcmp(k, "CanPlay") || !strcmp(k, "CanPause")) {
        b = *uri() != 0;
        variant(i, DBUS_TYPE_BOOLEAN, &b, "b");
    } else if (!strcmp(k, "CanControl")) {
        b = TRUE;
        variant(i, DBUS_TYPE_BOOLEAN, &b, "b");
    } else if (!strcmp(k, "CanSeek")) {
        b = gst.pipe != NULL || mock;
        variant(i, DBUS_TYPE_BOOLEAN, &b, "b");
    } else
        return 0;
    return 1;
}
static void dbus_emit(void) {
    if (!dbus)
        return;
    DBusMessage *m = dbus_message_new_signal(OBJ, PROPS, "PropertiesChanged");
    DBusMessageIter i, a, z;
    dbus_message_iter_init_append(m, &i);
    const char *iface = MP;
    dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &a);
    const char *s = playback();
    prop_basic(&a, "PlaybackStatus", DBUS_TYPE_STRING, &s, "s");
    prop_basic(&a, "Volume", DBUS_TYPE_DOUBLE, &volume, "d");
    metadata_prop(&a);
    const char *keys[] = {"CanGoNext", "CanGoPrevious", "CanSeek", "CanPlay", "CanPause", NULL};
    for (int n = 0; keys[n]; n++) {
        DBusMessageIter e;
        dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
        dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &keys[n]);
        get_property(&e, MP, keys[n]);
        dbus_message_iter_close_container(&a, &e);
    }
    dbus_message_iter_close_container(&i, &a);
    dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "s", &z);
    dbus_message_iter_close_container(&i, &z);
    dbus_connection_send(dbus, m, NULL);
    dbus_message_unref(m);
}
static void dbus_error(DBusMessage *m, const char *name, const char *text) {
    DBusMessage *r = dbus_message_new_error(m, name, text);
    if (r) {
        dbus_connection_send(dbus, r, NULL);
        dbus_message_unref(r);
    }
}
static void handle_dbus(DBusMessage *m) {
    if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return;
    const char *path = dbus_message_get_path(m), *iface = dbus_message_get_interface(m),
               *a = dbus_message_get_member(m);
    if (!path || strcmp(path, OBJ) || !iface || !a) {
        dbus_error(m, DBUS_ERROR_UNKNOWN_OBJECT, "Unknown object");
        return;
    }
    DBusMessage *r = dbus_message_new_method_return(m);
    DBusMessageIter i;
    dbus_message_iter_init_append(r, &i);
    int ok = 1;
    if (!strcmp(iface, PROPS)) {
        const char *in = NULL, *key = NULL;
        if (!strcmp(a, "Get")) {
            if (!dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &in, DBUS_TYPE_STRING, &key,
                                       DBUS_TYPE_INVALID) ||
                !get_property(&i, in, key))
                ok = 0;
        } else if (!strcmp(a, "GetAll")) {
            if (!dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &in, DBUS_TYPE_INVALID))
                ok = 0;
            else {
                DBusMessageIter ar;
                dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &ar);
                const char *pk[] = {"PlaybackStatus", "Metadata",    "Volume",      "Position",
                                    "Rate",           "MinimumRate", "MaximumRate", "CanGoNext",
                                    "CanGoPrevious",  "CanPlay",     "CanPause",    "CanSeek",
                                    "CanControl",     NULL};
                const char *rk[] = {"Identity",
                                    "CanQuit",
                                    "CanRaise",
                                    "HasTrackList",
                                    "SupportedUriSchemes",
                                    "SupportedMimeTypes",
                                    NULL};
                const char **p = !strcmp(in, ROOT) ? rk : pk;
                if (strcmp(in, ROOT) && strcmp(in, MP))
                    ok = 0;
                for (; ok && *p; p++) {
                    DBusMessageIter e;
                    dbus_message_iter_open_container(&ar, DBUS_TYPE_DICT_ENTRY, NULL, &e);
                    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, p);
                    get_property(&e, in, *p);
                    dbus_message_iter_close_container(&ar, &e);
                }
                dbus_message_iter_close_container(&i, &ar);
            }
        } else if (!strcmp(a, "Set")) {
            DBusMessageIter q, v;
            if (!dbus_message_iter_init(m, &q) ||
                dbus_message_iter_get_arg_type(&q) != DBUS_TYPE_STRING)
                ok = 0;
            else {
                dbus_message_iter_get_basic(&q, &in);
                dbus_message_iter_next(&q);
                if (dbus_message_iter_get_arg_type(&q) != DBUS_TYPE_STRING)
                    ok = 0;
                else {
                    dbus_message_iter_get_basic(&q, &key);
                    dbus_message_iter_next(&q);
                    if (dbus_message_iter_get_arg_type(&q) != DBUS_TYPE_VARIANT)
                        ok = 0;
                    else {
                        dbus_message_iter_recurse(&q, &v);
                        if (strcmp(in, MP) || strcmp(key, "Volume") ||
                            dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_DOUBLE)
                            ok = 0;
                        else {
                            double d;
                            dbus_message_iter_get_basic(&v, &d);
                            ok = set_volume(d);
                        }
                    }
                }
            }
        } else
            ok = 0;
    } else if (!strcmp(iface, MP)) {
        if (!strcmp(a, "Play"))
            ok = !*uri() || audio_play();
        else if (!strcmp(a, "Pause")) {
            if (state == 1)
                ok = audio_pause();
        } else if (!strcmp(a, "PlayPause"))
            ok = state == 1 ? audio_pause() : (!*uri() || audio_play());
        else if (!strcmp(a, "Stop"))
            audio_stop();
        else if (!strcmp(a, "Next")) {
            if (next_track.uri && *next_track.uri)
                ok = select_next(0);
        } else if (!strcmp(a, "Previous")) {
            if (nhistory)
                ok = select_next(1);
        } else if (!strcmp(a, "Seek")) {
            dbus_int64_t off;
            if (!dbus_message_get_args(m, NULL, DBUS_TYPE_INT64, &off, DBUS_TYPE_INVALID))
                ok = 0;
            else
                ok = seek_to(fmax(0, position() + off / 1e6));
        } else
            ok = 0;
    } else if (!strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(a, "Introspect")) {
        const char *s = MPRIS_XML;
        dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &s);
    } else if (!strcmp(iface, "org.freedesktop.DBus.Peer") && !strcmp(a, "Ping")) {
    } else
        ok = 0;
    if (ok)
        dbus_connection_send(dbus, r, NULL);
    else
        dbus_error(m, DBUS_ERROR_INVALID_ARGS, "Unsupported operation or invalid arguments");
    dbus_message_unref(r);
}
static int dbus_init(void) {
    if (no_dbus)
        return 1;
    DBusError e;
    dbus_error_init(&e);
    dbus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
    if (!dbus) {
        fprintf(stderr, "D-Bus: %s\n", e.message ? e.message : "failed");
        dbus_error_free(&e);
        return 0;
    }
    dbus_connection_set_exit_on_disconnect(dbus, FALSE);
    int r = dbus_bus_request_name(dbus, "org.mpris.MediaPlayer2.StationDLNA",
                                  DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        dbus_error_free(&e);
        return 0;
    }
    return 1;
}

/* HTTP: fixed connection count, nonblocking I/O, no chunked requests. */
typedef struct {
    int fd;
    struct in_addr peer;
    Buf input, output;
    size_t sent;
    double deadline;
    int ready;
} Client;
static Client clients[MAX_CLIENTS];
typedef struct {
    int used, service;
    char sid[80], path[256];
    struct in_addr ip;
    unsigned short port;
    unsigned seq;
    double expiry;
    int initial;
} Sub;
static Sub subs[MAX_SUBS];
static unsigned sid_counter;
static int nonblock(int fd) {
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
static void response(Client *c, int code, const char *reason, const char *type, const char *body,
                     const char *extra) {
    init(&c->output);
    size_t n = body ? strlen(body) : 0;
    fmt(&c->output,
        "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nContent-Type: %s\r\nConnection: close\r\nServer: "
        "Linux/4.9 UPnP/1.0 StationDLNA/" VERSION "\r\n%s\r\n",
        code, reason, n, type ? type : "text/plain", extra ? extra : "");
    if (n)
        addn(&c->output, body, n);
    c->ready = 1;
}
static int header(const char *s, size_t n, const char *key, char *out, size_t cap) {
    const char *p = memchr(s, '\n', n);
    if (!p)
        return 0;
    p++;
    size_t k = strlen(key);
    int found = 0;
    while (p < s + n) {
        const char *e = memchr(p, '\n', (size_t)(s + n - p));
        if (!e)
            break;
        if (e == p || (*p == '\r' && e == p + 1))
            break;
        if ((size_t)(e - p) > k && p[k] == ':' && !strncasecmp(p, key, k)) {
            if (found)
                return -1;
            const char *v = p + k + 1;
            while (v < e && (*v == ' ' || *v == '\t'))
                v++;
            const char *end = e;
            while (end > v && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t'))
                end--;
            size_t len = end - v;
            if (len >= cap)
                return -1;
            memcpy(out, v, len);
            out[len] = 0;
            found = 1;
        }
        p = e + 1;
    }
    return found;
}
static int service_path(const char *path, const char *prefix) {
    for (int s = 0; s < 3; s++) {
        char p[64];
        snprintf(p, sizeof(p), "/%s/%s", prefix,
                 s == 0   ? "avtransport"
                 : s == 1 ? "renderingcontrol"
                          : "connectionmanager");
        if (!strcmp(p, path))
            return s;
    }
    return -1;
}
static const char *service_type(int s) {
    return s == 0 ? AVT : s == 1 ? RCS : CMS;
}
static void last_change(int s, Buf *b) {
    if (s == 2) {
        add(b, "<e:propertyset "
               "xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property><SourceProtocolInfo/></"
               "e:property><e:property>");
        field(b, "SinkProtocolInfo", SINK_PROTOCOLS);
        add(b, "</e:property><e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></"
               "e:property></e:propertyset>");
        return;
    }
    Buf event;
    init(&event);
    fmt(&event, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/%s/\"><InstanceID val=\"0\">",
        s == 0 ? "AVT" : "RCS");
    if (s == 0) {
        fmt(&event,
            "<TransportState val=\"%s\"/><TransportStatus val=\"%s\"/><TransportPlaySpeed "
            "val=\"1\"/>",
            transport(), transport_error ? "ERROR_OCCURRED" : "OK");
        add(&event, "<AVTransportURI val=\"");
        esc(&event, uri());
        add(&event, "\"/><CurrentTrackURI val=\"");
        esc(&event, uri());
        add(&event, "\"/><AVTransportURIMetaData val=\"");
        esc(&event, meta());
        add(&event, "\"/><CurrentTrackMetaData val=\"");
        esc(&event, meta());
        char p[32];
        time_string(duration(), p);
        fmt(&event,
            "\"/><CurrentTrack val=\"%d\"/><NumberOfTracks val=\"%d\"/><CurrentTrackDuration "
            "val=\"%s\"/><CurrentPlayMode val=\"NORMAL\"/>",
            *uri() ? 1 : 0, *uri() ? 1 : 0, p);
        char a[80];
        actions(a);
        fmt(&event, "<CurrentTransportActions val=\"%s\"/>", a);
    } else
        fmt(&event, "<Volume channel=\"Master\" val=\"%d\"/><Mute channel=\"Master\" val=\"%d\"/>",
            (int)lround(volume * 100), muted);
    add(&event, "</InstanceID></Event>");
    add(b, "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\"><e:property>");
    field(b, "LastChange", event.s ? event.s : "");
    add(b, "</e:property></e:propertyset>");
    clear(&event);
}
static void send_event(Sub *s) {
    /* Outbound callbacks use the same bounded nonblocking client table. Never
     * connect to a host different from the subscribing peer (SSRF defence). */
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd < 0) {
            slot = i;
            break;
        }
    if (slot < 0)
        return;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    nonblock(fd);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr = s->ip, .sin_port = htons(s->port)};
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return;
    }
    Client *c = &clients[slot];
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->deadline = now() + 3;
    c->ready = 2;
    Buf body;
    init(&body);
    last_change(s->service, &body);
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &s->ip, ip, sizeof(ip));
    fmt(&c->output,
        "NOTIFY %s HTTP/1.1\r\nHOST: %s:%u\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\nNT: "
        "upnp:event\r\nNTS: upnp:propchange\r\nSID: %s\r\nSEQ: %u\r\nCONTENT-LENGTH: "
        "%zu\r\nConnection: close\r\n\r\n",
        s->path, ip, s->port, s->sid, s->seq++, body.n);
    add(&c->output, body.s ? body.s : "");
    clear(&body);
    s->initial = 0;
}
static void subscription(Client *c, const char *method, const char *path, size_t hn) {
    int service = service_path(path, "event");
    if (service < 0) {
        response(c, 404, "Not Found", NULL, "", NULL);
        return;
    }
    char sid[100], callback[512], nt[40], timeout[80];
    int hs = header(c->input.s, hn, "SID", sid, sizeof(sid)),
        hc = header(c->input.s, hn, "CALLBACK", callback, sizeof(callback)),
        ht = header(c->input.s, hn, "NT", nt, sizeof(nt));
    if (hs < 0 || hc < 0 || ht < 0) {
        response(c, 400, "Bad Request", NULL, "", NULL);
        return;
    }
    Sub *s = NULL;
    for (int i = 0; i < MAX_SUBS; i++)
        if (subs[i].used && subs[i].expiry > now() && hs && !strcmp(sid, subs[i].sid) &&
            service == subs[i].service && c->peer.s_addr == subs[i].ip.s_addr)
            s = &subs[i];
    if (!strcmp(method, "UNSUBSCRIBE")) {
        if (!s || hc || ht) {
            response(c, 412, "Precondition Failed", NULL, "", NULL);
            return;
        }
        s->used = 0;
        response(c, 200, "OK", NULL, "", NULL);
        return;
    }
    if (hs) {
        if (!s || hc || ht) {
            response(c, 412, "Precondition Failed", NULL, "", NULL);
            return;
        }
    } else {
        if (hc != 1 || ht != 1 || strcmp(nt, "upnp:event")) {
            response(c, 412, "Precondition Failed", NULL, "", NULL);
            return;
        }
        /* Only a single numeric HTTP URL, no DNS, userinfo, redirects or CRLF. */
        char ip[64], p[256], tail;
        unsigned cbport;
        if (sscanf(callback, "<http://%63[0-9.]:%u%255[^>]>%c", ip, &cbport, p, &tail) != 3 ||
            !cbport || cbport > 65535 || p[0] != '/') {
            response(c, 412, "Precondition Failed", NULL, "", NULL);
            return;
        }
        struct in_addr addr;
        if (inet_pton(AF_INET, ip, &addr) != 1 || addr.s_addr != c->peer.s_addr) {
            response(c, 412, "Precondition Failed", NULL, "", NULL);
            return;
        }
        for (char *q = p; *q; q++)
            if ((unsigned char)*q <= 32 || *q == 127) {
                response(c, 412, "Precondition Failed", NULL, "", NULL);
                return;
            }
        for (int i = 0; i < MAX_SUBS; i++)
            if (!subs[i].used || subs[i].expiry <= now()) {
                s = &subs[i];
                break;
            }
        if (!s) {
            response(c, 503, "Service Unavailable", NULL, "", NULL);
            return;
        }
        memset(s, 0, sizeof(*s));
        s->used = 1;
        s->service = service;
        s->ip = addr;
        s->port = cbport;
        strcpy(s->path, p);
        snprintf(s->sid, sizeof(s->sid), "uuid:%s-%u", uuid + 5, ++sid_counter);
        s->initial = 1;
    }
    unsigned seconds = 1800;
    if (header(c->input.s, hn, "TIMEOUT", timeout, sizeof(timeout)) == 1) {
        if (!strncasecmp(timeout, "Second-", 7)) {
            unsigned n;
            if (uint_arg(timeout + 7, 86400, &n))
                seconds = n < 30 ? 30 : n > 1800 ? 1800 : n;
        }
    }
    s->expiry = now() + seconds;
    char extra[160];
    snprintf(extra, sizeof(extra), "SID: %s\r\nTIMEOUT: Second-%u\r\n", s->sid, seconds);
    response(c, 200, "OK", NULL, "", extra);
}
static void route(Client *c, size_t hn, size_t bn) {
    char method[20], path[160], version[20], tail;
    if (sscanf(c->input.s, "%19s %159s %19s%c", method, path, version, &tail) != 4 ||
        strncmp(version, "HTTP/1.", 7)) {
        response(c, 400, "Bad Request", NULL, "", NULL);
        return;
    }
    if (!strcmp(method, "GET")) {
        if (!strcmp(path, "/description.xml")) {
            Buf b;
            init(&b);
            add(&b, "<?xml version=\"1.0\"?><root "
                    "xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</"
                    "major><minor>0</minor></"
                    "specVersion><device><deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</"
                    "deviceType>");
            field(&b, "friendlyName", friendly);
            field(&b, "manufacturer", "Station DLNA project");
            field(&b, "modelName", "Mini 2 lightweight audio renderer");
            field(&b, "modelNumber", VERSION);
            field(&b, "UDN", uuid);
            add(&b, DEVICE_SERVICES);
            add(&b, "</device></root>");
            response(c, 200, "OK", "text/xml; charset=utf-8", b.s, NULL);
            clear(&b);
            return;
        }
        for (int i = 0; i < 3; i++) {
            char p[64];
            snprintf(p, sizeof(p), "/scpd/%s.xml",
                     i == 0   ? "avtransport"
                     : i == 1 ? "renderingcontrol"
                              : "connectionmanager");
            if (!strcmp(p, path)) {
                response(c, 200, "OK", "text/xml; charset=utf-8", SCPDS[i], NULL);
                return;
            }
        }
        response(c, 404, "Not Found", NULL, "", NULL);
        return;
    }
    if (!strcmp(method, "SUBSCRIBE") || !strcmp(method, "UNSUBSCRIBE")) {
        subscription(c, method, path, hn);
        return;
    }
    if (strcmp(method, "POST")) {
        response(c, 405, "Method Not Allowed", NULL, "", NULL);
        return;
    }
    int service = service_path(path, "control");
    if (service < 0) {
        response(c, 404, "Not Found", NULL, "", NULL);
        return;
    }
    Soap x;
    int valid = parse_soap(&x, c->input.s + hn, bn);
    Buf result;
    init(&result);
    int err = valid ? 0 : 402;
    char action[256];
    if (!err) {
        if (header(c->input.s, hn, "SOAPACTION", action, sizeof(action)) != 1)
            err = 402;
        else {
            char expected[256];
            snprintf(expected, sizeof(expected), "\"%s#%s\"", service_type(service), x.action);
            if (strcmp(action, expected))
                err = 401;
        }
    }
    if (!err)
        err = soap_action(service, &x, &result);
    Buf body;
    init(&body);
    add(&body,
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>");
    if (err) {
        fmt(&body,
            "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</"
            "faultstring><detail><UPnPError "
            "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</"
            "errorCode><errorDescription>Action "
            "failed</errorDescription></UPnPError></detail></s:Fault>",
            err);
    } else {
        fmt(&body, "<u:%sResponse xmlns:u=\"%s\">", x.action, service_type(service));
        add(&body, result.s ? result.s : "");
        fmt(&body, "</u:%sResponse>", x.action);
    }
    add(&body, "</s:Body></s:Envelope>");
    if (body.bad)
        response(c, 500, "Internal Server Error", NULL, "", NULL);
    else
        response(c, err ? 500 : 200, err ? "Internal Server Error" : "OK",
                 "text/xml; charset=utf-8", body.s, NULL);
    free_soap(&x);
    clear(&result);
    clear(&body);
}
static void consume(Client *c) {
    char chunk[4096];
    for (int n = 0; n < 8; n++) {
        ssize_t r = recv(c->fd, chunk, sizeof(chunk), 0);
        if (r > 0)
            addn(&c->input, chunk, (size_t)r);
        else if (!r) {
            c->deadline = 0;
            return;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        else {
            c->deadline = 0;
            return;
        }
    }
    if (c->input.bad || c->input.n > 73728) {
        response(c, 413, "Payload Too Large", NULL, "", NULL);
        return;
    }
    if (!c->input.s)
        return;
    char *end = strstr(c->input.s, "\r\n\r\n");
    if (!end) {
        if (c->input.n > 8192)
            response(c, 431, "Request Header Fields Too Large", NULL, "", NULL);
        return;
    }
    size_t hn = end + 4 - c->input.s;
    if (hn > 8192) {
        response(c, 431, "Request Header Fields Too Large", NULL, "", NULL);
        return;
    }
    char v[80];
    if (header(c->input.s, hn, "Transfer-Encoding", v, sizeof(v))) {
        response(c, 400, "Bad Request", NULL, "", NULL);
        return;
    }
    unsigned length = 0;
    int h = header(c->input.s, hn, "Content-Length", v, sizeof(v));
    if (h < 0 || (h == 1 && !uint_arg(v, 65536, &length))) {
        response(c, 400, "Bad Request", NULL, "", NULL);
        return;
    }
    if (c->input.n >= hn + length)
        route(c, hn, length);
}
static void close_client(Client *c) {
    close(c->fd);
    clear(&c->input);
    clear(&c->output);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

/* SSDP responses are delayed inside the loop, not in one worker per search. */
typedef struct {
    int used;
    struct sockaddr_in to;
    char st[160];
    double due;
} Search;
static Search searches[16];
static const char *targets[] = {
    "upnp:rootdevice", NULL, "urn:schemas-upnp-org:device:MediaRenderer:1", AVT, RCS, CMS};
static void usn(Buf *b, const char *target) {
    add(b, uuid);
    if (strcmp(target, uuid)) {
        add(b, "::");
        add(b, target);
    }
}
static void ssdp_send(const char *target, struct sockaddr_in *to, const char *nts) {
    Buf b;
    init(&b);
    if (nts) {
        add(&b, "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n");
        fmt(&b, "NT: %s\r\nNTS: ssdp:%s\r\n", target, nts);
    } else
        fmt(&b, "HTTP/1.1 200 OK\r\nST: %s\r\nEXT:\r\n", target);
    add(&b, "USN: ");
    usn(&b, target);
    add(&b, "\r\n");
    if (!nts || strcmp(nts, "byebye"))
        fmt(&b,
            "CACHE-CONTROL: max-age=1800\r\nLOCATION: http://%s:%d/description.xml\r\nSERVER: "
            "Linux/4.9 UPnP/1.0 StationDLNA/" VERSION "\r\n",
            bind_ip, port);
    add(&b, "\r\n");
    sendto(ssdpfd, b.s, b.n, 0, (struct sockaddr *)to, sizeof(*to));
    clear(&b);
}
static void announce(const char *kind) {
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(1900)};
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    for (int i = 0; i < 6; i++)
        ssdp_send(targets[i] ? targets[i] : uuid, &to, kind);
}
static void receive_search(void) {
    for (int i = 0; i < 8; i++) {
        char data[2049];
        struct sockaddr_in from;
        socklen_t n = sizeof(from);
        ssize_t r = recvfrom(ssdpfd, data, sizeof(data) - 1, 0, (struct sockaddr *)&from, &n);
        if (r < 0)
            break;
        data[r] = 0;
        if ((from.sin_addr.s_addr & netmask.s_addr) != (local_addr.s_addr & netmask.s_addr) ||
            strncmp(data, "M-SEARCH * HTTP/1.1\r\n", 21))
            continue;
        char st[160], man[80], mx[16];
        if (header(data, r, "ST", st, sizeof(st)) != 1 ||
            header(data, r, "MAN", man, sizeof(man)) != 1 || strcmp(man, "\"ssdp:discover\"") ||
            header(data, r, "MX", mx, sizeof(mx)) != 1)
            continue;
        unsigned delay;
        if (!uint_arg(mx, 5, &delay) || !delay)
            continue;
        int matches = !strcmp(st, "ssdp:all");
        for (int j = 0; j < 6; j++)
            if (!strcmp(st, targets[j] ? targets[j] : uuid))
                matches = 1;
        if (!matches)
            continue;
        for (int j = 0; j < 16; j++)
            if (!searches[j].used) {
                searches[j].used = 1;
                searches[j].to = from;
                strcpy(searches[j].st, st);
                searches[j].due = now() + ((double)(rand() % 1000) / 1000) * delay;
                break;
            }
    }
}
static int network_init(void) {
    struct ifaddrs *all;
    if (getifaddrs(&all) < 0)
        return 0;
    int found = 0;
    for (struct ifaddrs *a = all; a; a = a->ifa_next)
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, ifname)) {
            local_addr = ((struct sockaddr_in *)a->ifa_addr)->sin_addr;
            netmask = ((struct sockaddr_in *)a->ifa_netmask)->sin_addr;
            found = 1;
            break;
        }
    freeifaddrs(all);
    if (!found) {
        logmsg("interface has no IPv4 address");
        return 0;
    }
    inet_ntop(AF_INET, &local_addr, bind_ip, sizeof(bind_ip));
    int one = 1;
    httpfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (httpfd < 0)
        return 0;
    setsockopt(httpfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr = local_addr, .sin_port = htons(port)};
    if (bind(httpfd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(httpfd, 8) < 0)
        return 0;
    nonblock(httpfd);
    ssdpfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ssdpfd < 0)
        return 0;
    setsockopt(ssdpfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(1900);
    if (bind(ssdpfd, (struct sockaddr *)&a, sizeof(a)) < 0)
        return 0;
    struct ip_mreq m;
    m.imr_interface = local_addr;
    inet_pton(AF_INET, "239.255.255.250", &m.imr_multiaddr);
    if (setsockopt(ssdpfd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) < 0)
        return 0;
    setsockopt(ssdpfd, IPPROTO_IP, IP_MULTICAST_IF, &local_addr, sizeof(local_addr));
    unsigned char ttl = 2;
    setsockopt(ssdpfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    nonblock(ssdpfd);
    return 1;
}
static void accept_clients(void) {
    for (int n = 0; n < 8; n++) {
        struct sockaddr_in a;
        socklen_t l = sizeof(a);
        int fd = accept4(httpfd, (struct sockaddr *)&a, &l, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
            break;
        int slot = -1;
        if ((a.sin_addr.s_addr & netmask.s_addr) == (local_addr.s_addr & netmask.s_addr))
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd < 0) {
                    slot = i;
                    break;
                }
        if (slot < 0) {
            close(fd);
            continue;
        }
        Client *c = &clients[slot];
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->peer = a.sin_addr;
        c->deadline = now() + 5;
    }
}
static void loop(void) {
    double advert = 0, mixer = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i].fd = -1;
    while (running) {
        double t = now();
        if (t >= advert) {
            announce("alive");
            advert = t + 600;
        }
        if (dbus) {
            dbus_connection_read_write(dbus, 0);
            if (!dbus_connection_get_is_connected(dbus)) {
                logmsg("private D-Bus disconnected; exiting for supervisor");
                break;
            }
            for (int n = 0; n < 16; n++) {
                DBusMessage *m = dbus_connection_pop_message(dbus);
                if (!m)
                    break;
                handle_dbus(m);
                dbus_message_unref(m);
            }
        }
        if (atomic_exchange(&gst_error, 0)) {
            char error[180];
            snprintf(error, sizeof(error),
                     "audio pipeline failed: domain=%d code=%d transport=%d http=%d",
                     atomic_load(&gst_error_domain), atomic_load(&gst_error_code),
                     atomic_load(&media_proxy.error), atomic_load(&media_proxy.http_status));
            logmsg(error);
            audio_stop();
            transport_error = 1;
        } else if (atomic_exchange(&gst_eos, 0)) {
            if (!select_next(0))
                audio_stop();
        }
        if (atomic_exchange(&gst_playing, 0) && state == 3) {
            state = 1;
            changed = 1;
        }
        if (t >= mixer) {
            mixer_poll();
            mixer = t + 0.5;
        }
        if (changed) {
            dbus_emit();
            for (int i = 0; i < MAX_SUBS; i++)
                if (subs[i].used && subs[i].expiry > t)
                    send_event(&subs[i]);
            changed = 0;
        }
        for (int i = 0; i < MAX_SUBS; i++)
            if (subs[i].used && subs[i].initial && subs[i].expiry > t)
                send_event(&subs[i]);
        for (int i = 0; i < 16; i++)
            if (searches[i].used && searches[i].due <= t) {
                if (!strcmp(searches[i].st, "ssdp:all")) {
                    for (int j = 0; j < 6; j++)
                        ssdp_send(targets[j] ? targets[j] : uuid, &searches[i].to, NULL);
                } else
                    ssdp_send(searches[i].st, &searches[i].to, NULL);
                searches[i].used = 0;
            }
        struct pollfd f[MAX_CLIENTS + 2];
        f[0] = (struct pollfd){httpfd, POLLIN, 0};
        f[1] = (struct pollfd){ssdpfd, POLLIN, 0};
        for (int i = 0; i < MAX_CLIENTS; i++)
            f[i + 2] = (struct pollfd){clients[i].fd, clients[i].ready ? POLLOUT : POLLIN, 0};
        if (poll(f, MAX_CLIENTS + 2, 50) < 0 && errno != EINTR)
            break;
        if (f[0].revents & POLLIN)
            accept_clients();
        if (f[1].revents & POLLIN)
            receive_search();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            Client *c = &clients[i];
            if (c->fd < 0)
                continue;
            short re = f[i + 2].revents;
            if (re & (POLLERR | POLLHUP | POLLNVAL) || c->deadline < now()) {
                close_client(c);
                continue;
            }
            if (!c->ready && (re & POLLIN))
                consume(c);
            if (c->ready && (re & POLLOUT)) {
                int e = 0;
                socklen_t z = sizeof(e);
                if (c->ready == 2 && (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &e, &z) < 0 || e)) {
                    close_client(c);
                    continue;
                }
                if (c->output.bad) {
                    close_client(c);
                    continue;
                }
                ssize_t n = send(c->fd, c->output.s + c->sent, c->output.n - c->sent, MSG_NOSIGNAL);
                if (n > 0)
                    c->sent += n;
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close_client(c);
                    continue;
                }
                if (c->sent == c->output.n)
                    close_client(c);
            }
        }
    }
    announce("byebye");
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0)
            close_client(&clients[i]);
}
static void make_uuid(void) {
    char macpath[128], mac[80] = "";
    snprintf(macpath, sizeof(macpath), "/sys/class/net/%s/address", ifname);
    FILE *f = fopen(macpath, "r");
    if (f) {
        if (!fgets(mac, sizeof(mac), f))
            mac[0] = 0;
        fclose(f);
    }
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = mac; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 1099511628211ULL;
    }
    snprintf(uuid, sizeof(uuid), "uuid:8b9b8b20-1d11-4d10-9000-%012llx",
             (unsigned long long)(h & 0xffffffffffffULL));
}
static void usage(void) {
    puts("station-dlna " VERSION
         "\nUsage: station-dlna [--interface wlan0] [--port 49494] [--name NAME]\n  [--control "
         "'Master Playback Volume'] [--check] [--mock-audio] [--no-dbus]\nDBUS_SYSTEM_BUS_ADDRESS "
         "selects private Bluetooth bus. IPv4 audio renderer.\n--check checks audio ABI/control "
         "without creating a pipeline or changing volume.");
}
int main(int argc, char **argv) {
    int check = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
        if (!strcmp(argv[i], "--version")) {
            puts(VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (!strcmp(argv[i], "--mock-audio"))
            mock = 1;
        else if (!strcmp(argv[i], "--mock-mixer"))
            mock_mixer = 1;
        else if (!strcmp(argv[i], "--no-dbus"))
            no_dbus = 1;
        else if (i + 1 < argc && !strcmp(argv[i], "--interface")) {
            if (strlen(argv[++i]) >= sizeof(ifname))
                return 2;
            strcpy(ifname, argv[i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--name")) {
            if (strlen(argv[++i]) >= sizeof(friendly))
                return 2;
            strcpy(friendly, argv[i]);
        } else if (i + 1 < argc && !strcmp(argv[i], "--port")) {
            unsigned n;
            if (!uint_arg(argv[++i], 65535, &n) || n < 1024)
                return 2;
            port = n;
        } else if (i + 1 < argc && !strcmp(argv[i], "--control"))
            control_name = argv[++i];
        else {
            usage();
            return 2;
        }
    }
    signal(SIGINT, sigstop);
    signal(SIGTERM, sigstop);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)getpid());
    make_uuid();
    if (!audio_init()) {
        logmsg("audio ABI check failed");
        return 3;
    }
    if (!mixer_init()) {
        logmsg("existing ALSA volume control unavailable; refusing to create/change controls");
        return 4;
    }
    if (check) {
        puts(mock || mock_mixer
                 ? "Audio symbols: OK (mixer mocked; no playback started)"
                 : "Audio symbols and existing ALSA control: OK (no playback started)");
        if (ctl)
            snd_ctl_close(ctl);
        if (ctl_id)
            snd_ctl_elem_id_free(ctl_id);
        return 0;
    }
    if (!dbus_init())
        return 5;
    if (!network_init()) {
        perror("network setup");
        return 6;
    }
    fprintf(stderr, "station-dlna " VERSION ": %s:%d, %s audio\n", bind_ip, port,
            mock ? "mock" : "native");
    loop();
    audio_stop();
    if (ctl)
        snd_ctl_close(ctl);
    if (ctl_id)
        snd_ctl_elem_id_free(ctl_id);
    if (dbus) {
        dbus_connection_close(dbus);
        dbus_connection_unref(dbus);
    }
    if (httpfd >= 0)
        close(httpfd);
    if (ssdpfd >= 0)
        close(ssdpfd);
    free_track(&current);
    free_track(&next_track);
    for (unsigned i = 0; i < nhistory; i++)
        free_track(&history[i]);
    return 0;
}
