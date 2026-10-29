/*
 * Streaming LZFSE decoder on top of the reference decoder (lzfse_decode).
 * Shaped after Apple's compression_stream: feed src, drain dst, repeat.
 * Memory stays bounded: the 256 KB match history, one output chunk and one
 * encoded block, whatever the size of the whole stream.
 */

#ifndef LZFSE_STREAM_H
#define LZFSE_STREAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  /* Progress made; feed more src or give more dst space. */
  LZFSE_STREAM_STATUS_OK = 0,
  /* End of stream decoded and every decoded byte written to dst. */
  LZFSE_STREAM_STATUS_END = 1,
  LZFSE_STREAM_STATUS_ERROR = -1,
} lzfse_stream_status;

typedef struct {
  /* Next byte to write and space left; advanced by lzfse_stream_process. */
  uint8_t *dst_ptr;
  size_t dst_size;
  /* Next byte to read and bytes left; advanced by lzfse_stream_process. */
  const uint8_t *src_ptr;
  size_t src_size;
  /* Owned by the stream, do not touch. */
  void *state;
} lzfse_stream;

lzfse_stream_status lzfse_stream_init(lzfse_stream *stream);

/*
 * Consumes as much src as it can and writes as much dst as fits.
 * Pass finalize != 0 once the whole input has been given; a truncated stream
 * then fails instead of waiting for more src.
 */
lzfse_stream_status lzfse_stream_process(lzfse_stream *stream, int finalize);

void lzfse_stream_destroy(lzfse_stream *stream);

#ifdef __cplusplus
}
#endif

#endif /* LZFSE_STREAM_H */
