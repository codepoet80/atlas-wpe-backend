/* Atlas: minimal GStreamer 1.x audio sink that routes through PulseAudio via the stable pa_simple
 * API. The device's PulseAudio is 0.9.22 (too old for the gst-1.20 pulse plugin, which needs
 * libpulse >= 2.0), but pa_simple is ABI-stable and routes to audiod -> speaker. Registered as
 * Sink/Audio with a high rank so WebKit's autoaudiosink picks it.
 *
 * THREAD-SAFETY / LOCKUP FIX (2026-07-08): pa_simple is NOT safe for concurrent use from multiple
 * threads, but GstAudioSink calls ::reset (from the state-change/streaming thread, to unblock a
 * blocking write on flush/stop) CONCURRENTLY with ::write (on the ringbuffer thread). The old code
 * called pa_simple_flush() in ::reset while ::write was inside pa_simple_write() -> concurrent
 * pa_simple access -> hard hang on navigate-away-during-playback (locked the whole device). Fix:
 * a GMutex serialises ALL pa_simple access so flush/free never run concurrently with a write, plus
 * an atomic `flushing` flag so once teardown/flush starts, the ringbuffer thread DROPS further
 * writes instead of re-blocking. A write already in flight drains naturally (pulse keeps draining
 * during a normal stop) and returns, then reset/unprepare proceed under the lock. */
#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudiosink.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <string.h>

#define GST_TYPE_PA_SINK (gst_pa_sink_get_type())
G_DECLARE_FINAL_TYPE(GstPaSink, gst_pa_sink, GST, PA_SINK, GstAudioSink)

struct _GstPaSink {
    GstAudioSink parent;
    pa_simple   *pa;
    pa_sample_spec ss;
    guint        bpf;      /* bytes per frame */
    GMutex       lock;     /* serialises ALL pa_simple access across threads */
    gint         flushing; /* atomic: set in reset, cleared after flush/in prepare */
};

G_DEFINE_TYPE(GstPaSink, gst_pa_sink, GST_TYPE_AUDIO_SINK)

static GstStaticPadTemplate sink_tmpl = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS("audio/x-raw, "
        "format = (string) { S16LE, S16BE, F32LE, S32LE, U8 }, "
        "rate = (int) [ 1, MAX ], channels = (int) [ 1, 8 ], "
        "layout = (string) interleaved"));

static gboolean gst_pa_sink_open(GstAudioSink *s)  { (void)s; return TRUE; }   /* device opened lazily in prepare */
static gboolean gst_pa_sink_close(GstAudioSink *s) { (void)s; return TRUE; }

static gboolean gst_pa_sink_prepare(GstAudioSink *asink, GstAudioRingBufferSpec *spec)
{
    GstPaSink *self = GST_PA_SINK(asink);
    GstAudioInfo *info = &spec->info;
    pa_sample_format_t fmt = PA_SAMPLE_INVALID;
    switch (GST_AUDIO_INFO_FORMAT(info)) {
        case GST_AUDIO_FORMAT_S16LE: fmt = PA_SAMPLE_S16LE;      break;
        case GST_AUDIO_FORMAT_S16BE: fmt = PA_SAMPLE_S16BE;      break;
        case GST_AUDIO_FORMAT_F32LE: fmt = PA_SAMPLE_FLOAT32LE;  break;
        case GST_AUDIO_FORMAT_S32LE: fmt = PA_SAMPLE_S32LE;      break;
        case GST_AUDIO_FORMAT_U8:    fmt = PA_SAMPLE_U8;         break;
        default: GST_ERROR_OBJECT(self, "unsupported format"); return FALSE;
    }

    g_mutex_lock(&self->lock);
    self->ss.format   = fmt;
    self->ss.rate     = GST_AUDIO_INFO_RATE(info);
    self->ss.channels = GST_AUDIO_INFO_CHANNELS(info);
    self->bpf         = GST_AUDIO_INFO_BPF(info);
    g_atomic_int_set(&self->flushing, 0);

    int err = 0;
    self->pa = pa_simple_new(NULL, "AtlasBrowser", PA_STREAM_PLAYBACK, NULL, "webkit",
                             &self->ss, NULL, NULL, &err);
    gboolean ok = (self->pa != NULL);
    g_mutex_unlock(&self->lock);

    if (!ok) {
        GST_ERROR_OBJECT(self, "pa_simple_new failed: %s", pa_strerror(err));
        return FALSE;
    }
    GST_INFO_OBJECT(self, "pa_simple opened %dch %dHz fmt=%d", self->ss.channels, self->ss.rate, fmt);
    return TRUE;
}

static gboolean gst_pa_sink_unprepare(GstAudioSink *asink)
{
    GstPaSink *self = GST_PA_SINK(asink);
    /* Waits (via the lock) for any in-flight write to finish, then frees. Safe: no concurrent pa call. */
    g_atomic_int_set(&self->flushing, 1);
    g_mutex_lock(&self->lock);
    if (self->pa) { pa_simple_free(self->pa); self->pa = NULL; }
    g_mutex_unlock(&self->lock);
    return TRUE;
}

static gint gst_pa_sink_write(GstAudioSink *asink, gpointer data, guint length)
{
    GstPaSink *self = GST_PA_SINK(asink);
    int err = 0;
    /* Dropped while a flush/teardown is in progress so we don't re-block and stall the ringbuffer
     * thread while ::reset/::unprepare want the lock. Returning `length` claims the buffer as consumed. */
    if (g_atomic_int_get(&self->flushing))
        return (gint)length;

    g_mutex_lock(&self->lock);
    if (!self->pa) { g_mutex_unlock(&self->lock); return -1; }
    gint rc = pa_simple_write(self->pa, data, (size_t)length, &err) < 0 ? -1 : (gint)length;
    g_mutex_unlock(&self->lock);

    if (rc < 0)
        GST_ERROR_OBJECT(self, "pa_simple_write failed: %s", pa_strerror(err));
    return rc;   /* blocking write consumes it all */
}

static guint gst_pa_sink_delay(GstAudioSink *asink)
{
    GstPaSink *self = GST_PA_SINK(asink);
    int err = 0;
    guint frames = 0;
    g_mutex_lock(&self->lock);
    if (self->pa && self->bpf) {
        pa_usec_t usec = pa_simple_get_latency(self->pa, &err);
        if (!err)
            frames = (guint)(((guint64)usec * self->ss.rate) / 1000000ULL);   /* usec -> frames */
    }
    g_mutex_unlock(&self->lock);
    return frames;
}

static void gst_pa_sink_reset(GstAudioSink *asink)
{
    GstPaSink *self = GST_PA_SINK(asink);
    int err = 0;
    /* Stop feeding first: the ringbuffer thread now drops writes (see ::write), so a write in flight
     * drains and returns, letting us take the lock. Then flush is safe (no concurrent write). */
    g_atomic_int_set(&self->flushing, 1);
    g_mutex_lock(&self->lock);
    if (self->pa) pa_simple_flush(self->pa, &err);
    g_mutex_unlock(&self->lock);
    /* Allow writes to resume (e.g. after a flushing seek); on teardown the ringbuffer thread has
     * already stopped, so this is harmless there. */
    g_atomic_int_set(&self->flushing, 0);
}

static void gst_pa_sink_finalize(GObject *o)
{
    GstPaSink *self = GST_PA_SINK(o);
    if (self->pa) { pa_simple_free(self->pa); self->pa = NULL; }
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_pa_sink_parent_class)->finalize(o);
}

static void gst_pa_sink_class_init(GstPaSinkClass *klass)
{
    GObjectClass *go = G_OBJECT_CLASS(klass);
    GstElementClass *ec = GST_ELEMENT_CLASS(klass);
    GstAudioSinkClass *ac = GST_AUDIO_SINK_CLASS(klass);
    go->finalize = gst_pa_sink_finalize;
    gst_element_class_add_static_pad_template(ec, &sink_tmpl);
    gst_element_class_set_static_metadata(ec, "Atlas PulseAudio sink", "Sink/Audio",
        "Outputs to PulseAudio via pa_simple (webOS audiod)", "Atlas");
    ac->open = gst_pa_sink_open;   ac->close = gst_pa_sink_close;
    ac->prepare = gst_pa_sink_prepare;  ac->unprepare = gst_pa_sink_unprepare;
    ac->write = gst_pa_sink_write;  ac->delay = gst_pa_sink_delay;  ac->reset = gst_pa_sink_reset;
}

static void gst_pa_sink_init(GstPaSink *self)
{
    self->pa = NULL;
    self->bpf = 0;
    g_mutex_init(&self->lock);
    g_atomic_int_set(&self->flushing, 0);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    /* PRIMARY+20 so WebKit's autoaudiosink prefers it over any fallback */
    return gst_element_register(plugin, "atlaspasink", GST_RANK_PRIMARY + 20, GST_TYPE_PA_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, atlaspasink,
    "Atlas PulseAudio (pa_simple) audio sink", plugin_init, "1.0", "LGPL",
    "atlas", "atlas-browser")
