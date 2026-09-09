#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

// ----- EBML helpers (VINT) -----
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

// ----- Element parser -----
typedef struct {
    uint64_t id;
    uint64_t size;
    const uint8_t *data;   // points inside the original buffer
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
#define ID_TRACKS      0x1654AE6B
#define ID_CLUSTER     0x1F43B675
#define ID_BLOCK       0xA1
#define ID_SIMPLEBLOCK 0xA3
#define ID_CODEC       0x86
#define ID_WIDTH       0xB0
#define ID_HEIGHT      0xBA

// ----- Frame storage -----
#define MAX_FRAMES 5000
static const uint8_t *frame_data[MAX_FRAMES];
static size_t frame_len[MAX_FRAMES];
static int frame_count = 0;
static char codec[64] = "Unknown";
static int width = 0, height = 0;

// ----- Parse MKV (recursive descent, no mallocs) -----
static void parse_mkv_internal(const uint8_t *buf, size_t len);

static void parse_children(const uint8_t *buf, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        ebml_elem e;
        int used = parse_elem(buf + pos, len - pos, &e);
        if (!used) break;
        pos += used;

        // If it has data, parse it as children
        if (e.size > 0 && e.data + e.size <= buf + len) {
            parse_mkv_internal(e.data, e.size);
        }
    }
}

static void parse_mkv_internal(const uint8_t *buf, size_t len) {
    size_t pos = 0;
    while (pos < len) {
        ebml_elem e;
        int used = parse_elem(buf + pos, len - pos, &e);
        if (!used) break;
        pos += used;

        // Handle specific IDs
        switch (e.id) {
            case ID_TRACKS:
                // inside Tracks: find video codec + width/height
                {
                    size_t p = 0;
                    while (p < e.size) {
                        ebml_elem t;
                        int u = parse_elem(e.data + p, e.size - p, &t);
                        if (!u) break;
                        p += u;

                        if (t.id == 0x63A2) { // TrackEntry
                            // scan inside TrackEntry for codec, width, height
                            size_t q = 0;
                            while (q < t.size) {
                                ebml_elem f;
                                int u2 = parse_elem(t.data + q, t.size - q, &f);
                                if (!u2) break;
                                q += u2;

                                if (f.id == ID_CODEC && f.size < sizeof(codec)) {
                                    memcpy(codec, f.data, f.size);
                                    codec[f.size] = 0;
                                }
                                if (f.id == ID_WIDTH && f.size >= 2) {
                                    width = (f.data[0] << 8) | f.data[1];
                                }
                                if (f.id == ID_HEIGHT && f.size >= 2) {
                                    height = (f.data[0] << 8) | f.data[1];
                                }
                            }
                        }
                    }
                }
                break;

            case ID_CLUSTER:
                // inside Cluster: find SimpleBlock / Block
                {
                    size_t p = 0;
                    while (p < e.size) {
                        ebml_elem b;
                        int u = parse_elem(e.data + p, e.size - p, &b);
                        if (!u) break;
                        p += u;

                        if (b.id == ID_SIMPLEBLOCK || b.id == ID_BLOCK) {
                            if (frame_count < MAX_FRAMES && b.size > 4) {
                                // skip first 4 bytes (track, timecode, flags)
                                frame_data[frame_count] = b.data + 4;
                                frame_len[frame_count] = b.size - 4;
                                frame_count++;
                            }
                        }
                    }
                }
                break;

            default:
                // other elements: parse their children if they contain data
                if (e.size > 0) {
                    parse_mkv_internal(e.data, e.size);
                }
                break;
        }
    }
}

// ----- Exported functions for WASM -----
EMSCRIPTEN_KEEPALIVE
void parse_mkv(const uint8_t *data, size_t length) {
    frame_count = 0;
    width = height = 0;
    codec[0] = 0;
    parse_mkv_internal(data, length);
}

EMSCRIPTEN_KEEPALIVE
int get_frame_count(void) {
    return frame_count;
}

EMSCRIPTEN_KEEPALIVE
const uint8_t* get_frame_data(int index) {
    if (index < 0 || index >= frame_count) return NULL;
    return frame_data[index];
}

EMSCRIPTEN_KEEPALIVE
size_t get_frame_size(int index) {
    if (index < 0 || index >= frame_count) return 0;
    return frame_len[index];
}

EMSCRIPTEN_KEEPALIVE
const char* get_codec(void) {
    return codec;
}

EMSCRIPTEN_KEEPALIVE
int get_width(void) {
    return width;
}

EMSCRIPTEN_KEEPALIVE
int get_height(void) {
    return height;
}
