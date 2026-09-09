#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ----- EBML VINT -----
static uint64_t read_vint(const uint8_t *data, int *bytes) {
    int len = 1;
    uint64_t mask = 0x80;
    while (!(data[0] & mask) && len < 8) {
        mask >>= 1;
        len++;
    }
    uint64_t val = data[0] & (mask - 1);
    for (int i = 1; i < len; i++) {
        val = (val << 8) | data[i];
    }
    *bytes = len;
    return val;
}

// ----- EBML Element -----
typedef struct {
    uint64_t id;
    uint64_t size;
    const uint8_t *data;
} ebml_elem;

static int parse_elem(const uint8_t *buf, size_t max, ebml_elem *out) {
    int id_len, size_len;
    out->id = read_vint(buf, &id_len);
    if (id_len >= max) return 0;
    out->size = read_vint(buf + id_len, &size_len);
    if (id_len + size_len > max) return 0;
    out->data = buf + id_len + size_len;
    return id_len + size_len;
}

// ----- MKV IDs -----
#define ID_SEGMENT      0x18538067
#define ID_TRACKS       0x1654AE6B
#define ID_CLUSTER      0x1F43B675
#define ID_BLOCK        0xA1
#define ID_SIMPLEBLOCK  0xA3
#define ID_TRACK_ENTRY  0x63A2
#define ID_TRACK_TYPE   0x83
#define ID_CODEC_ID     0x86
#define ID_PIXEL_WIDTH  0xB0
#define ID_PIXEL_HEIGHT 0xBA
#define ID_SAMPLING_FREQ 0xB5
#define ID_CHANNELS     0x9F
#define ID_TRACK_NUMBER 0xD7

// ----- Storage -----
#define MAX_FRAMES 20000
static const uint8_t *video_frames[MAX_FRAMES];
static size_t video_frame_len[MAX_FRAMES];
static int video_count = 0;

static const uint8_t *audio_frames[MAX_FRAMES];
static size_t audio_frame_len[MAX_FRAMES];
static int audio_count = 0;

static char video_codec[64] = "Unknown";
static char audio_codec[64] = "Unknown";
static int width = 0, height = 0;
static int sample_rate = 0, channels = 0;
static uint64_t video_track_num = 0;
static uint64_t audio_track_num = 0;

// ----- Parse internals -----
static void parse_children(const uint8_t *buf, size_t len);

static void parse_mkv_internal(const uint8_t *buf, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        ebml_elem e;
        int used = parse_elem(buf + pos, len - pos, &e);
        if (!used) break;
        pos += used;

        if (e.id == ID_TRACKS) {
            // Parse tracks
            size_t p = 0;
            while (p < e.size) {
                ebml_elem t;
                int u = parse_elem(e.data + p, e.size - p, &t);
                if (!u) break;
                p += u;

                if (t.id == ID_TRACK_ENTRY) {
                    uint64_t track_num = 0;
                    int track_type = 0;
                    char codec[64] = {0};
                    int w = 0, h = 0, sr = 0, ch = 0;

                    size_t q = 0;
                    while (q < t.size) {
                        ebml_elem f;
                        int u2 = parse_elem(t.data + q, t.size - q, &f);
                        if (!u2) break;
                        q += u2;

                        if (f.id == ID_TRACK_NUMBER && f.size > 0) {
                            track_num = f.data[0];
                        }
                        if (f.id == ID_TRACK_TYPE && f.size > 0) {
                            track_type = f.data[0];
                        }
                        if (f.id == ID_CODEC_ID && f.size < sizeof(codec)) {
                            memcpy(codec, f.data, f.size);
                            codec[f.size] = 0;
                        }
                        if (f.id == ID_PIXEL_WIDTH && f.size >= 2) {
                            w = (f.data[0] << 8) | f.data[1];
                        }
                        if (f.id == ID_PIXEL_HEIGHT && f.size >= 2) {
                            h = (f.data[0] << 8) | f.data[1];
                        }
                        if (f.id == ID_SAMPLING_FREQ && f.size >= 4) {
                            sr = *(float*)(f.data);
                        }
                        if (f.id == ID_CHANNELS && f.size > 0) {
                            ch = f.data[0];
                        }
                    }

                    if (track_type == 1) {
                        video_track_num = track_num;
                        strcpy(video_codec, codec);
                        width = w;
                        height = h;
                    }
                    if (track_type == 2) {
                        audio_track_num = track_num;
                        strcpy(audio_codec, codec);
                        sample_rate = sr;
                        channels = ch;
                    }
                }
            }
        }

        if (e.id == ID_CLUSTER) {
            size_t p = 0;
            while (p < e.size) {
                ebml_elem b;
                int u = parse_elem(e.data + p, e.size - p, &b);
                if (!u) break;
                p += u;

                if (b.id == ID_SIMPLEBLOCK || b.id == ID_BLOCK) {
                    if (b.size < 4) continue;
                    uint64_t track_num = b.data[0];
                    
                    // Video frame
                    if (track_num == video_track_num && video_count < MAX_FRAMES) {
                        video_frames[video_count] = b.data + 4;
                        video_frame_len[video_count] = b.size - 4;
                        video_count++;
                    }
                    
                    // Audio frame
                    if (track_num == audio_track_num && audio_count < MAX_FRAMES) {
                        audio_frames[audio_count] = b.data + 4;
                        audio_frame_len[audio_count] = b.size - 4;
                        audio_count++;
                    }
                }
            }
        }

        // Recurse into children
        if (e.size > 0 && e.id != ID_TRACKS && e.id != ID_CLUSTER) {
            parse_mkv_internal(e.data, e.size);
        }
    }
}

// ============================================================
//  EXPORTED FUNCTIONS
// ============================================================

EMSCRIPTEN_KEEPALIVE
void parse_mkv(const uint8_t *data, size_t length) {
    video_count = 0;
    audio_count = 0;
    video_track_num = 0;
    audio_track_num = 0;
    width = height = 0;
    sample_rate = channels = 0;
    video_codec[0] = 0;
    audio_codec[0] = 0;
    parse_mkv_internal(data, length);
}

EMSCRIPTEN_KEEPALIVE int get_video_frame_count(void) { return video_count; }
EMSCRIPTEN_KEEPALIVE const uint8_t* get_video_frame(int i) { return (i < video_count) ? video_frames[i] : NULL; }
EMSCRIPTEN_KEEPALIVE size_t get_video_frame_size(int i) { return (i < video_count) ? video_frame_len[i] : 0; }
EMSCRIPTEN_KEEPALIVE int get_audio_frame_count(void) { return audio_count; }
EMSCRIPTEN_KEEPALIVE const uint8_t* get_audio_frame(int i) { return (i < audio_count) ? audio_frames[i] : NULL; }
EMSCRIPTEN_KEEPALIVE size_t get_audio_frame_size(int i) { return (i < audio_count) ? audio_frame_len[i] : 0; }
EMSCRIPTEN_KEEPALIVE const char* get_video_codec(void) { return video_codec; }
EMSCRIPTEN_KEEPALIVE const char* get_audio_codec(void) { return audio_codec; }
EMSCRIPTEN_KEEPALIVE int get_width(void) { return width; }
EMSCRIPTEN_KEEPALIVE int get_height(void) { return height; }
EMSCRIPTEN_KEEPALIVE int get_sample_rate(void) { return sample_rate; }
EMSCRIPTEN_KEEPALIVE int get_channels(void) { return channels; }
