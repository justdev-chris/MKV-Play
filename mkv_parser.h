#ifndef MKV_PARSER_H
#define MKV_PARSER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ----- Main entry point -----
void parse_mkv(const uint8_t *data, size_t length);

// ----- Video -----
int get_video_frame_count(void);
const uint8_t* get_video_frame(int index);
size_t get_video_frame_size(int index);
const char* get_video_codec(void);

// ----- Audio -----
int get_audio_frame_count(void);
const uint8_t* get_audio_frame(int index);
size_t get_audio_frame_size(int index);
const char* get_audio_codec(void);

// ----- Metadata -----
int get_width(void);
int get_height(void);
int get_sample_rate(void);
int get_channels(void);

#ifdef __cplusplus
}
#endif

#endif
