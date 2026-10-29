/*
 * Streaming LZFSE decoder, see include/lzfse_stream.h.
 *
 * The reference decoder resolves a match by reading back from dst_begin, up
 * to LZFSE_STREAM_HISTORY bytes. So decoded bytes go to an internal buffer,
 * and only the last LZFSE_STREAM_HISTORY of them are kept when it is full.
 * Its block states hold no pointer into src or dst, only counters and the
 * offset from src, so both buffers may move between calls.
 */

#include "lzfse_stream.h"
#include "lzfse_internal.h"

#include <stdlib.h>
#include <string.h>

/* Larger than any match distance: LZFSE 262139, LZVN 65535. */
#define LZFSE_STREAM_HISTORY ((size_t)1 << 18)
#define LZFSE_STREAM_OUTPUT_CHUNK ((size_t)1 << 20)
#define LZFSE_STREAM_INITIAL_INPUT ((size_t)1 << 20)

typedef struct {
  lzfse_decoder_state decoder;

  /* Encoded bytes; [input_pos, input_len) are not consumed yet. */
  uint8_t *input;
  size_t input_capacity;
  size_t input_pos;
  size_t input_len;

  /* Decoded bytes; [output_flushed, output_len) are not given to dst yet. */
  uint8_t *output;
  size_t output_capacity;
  size_t output_len;
  size_t output_flushed;

  int finished;
} lzfse_stream_state;

static void lzfse_stream_flush(lzfse_stream *stream, lzfse_stream_state *s) {
  size_t pending = s->output_len - s->output_flushed;
  size_t size = pending < stream->dst_size ? pending : stream->dst_size;
  if (size == 0)
    return;
  memcpy(stream->dst_ptr, s->output + s->output_flushed, size);
  stream->dst_ptr += size;
  stream->dst_size -= size;
  s->output_flushed += size;
}

/* Returns 0 on success, -1 when the buffer cannot grow. */
static int lzfse_stream_absorb(lzfse_stream *stream, lzfse_stream_state *s) {
  size_t unconsumed = s->input_len - s->input_pos;
  if (s->input_pos > 0) {
    memmove(s->input, s->input + s->input_pos, unconsumed);
    s->input_pos = 0;
    s->input_len = unconsumed;
  }

  // A whole encoded block must be present, so the buffer grows to the block.
  if (s->input_len == s->input_capacity && stream->src_size > 0) {
    size_t capacity = s->input_capacity * 2;
    uint8_t *input = (uint8_t *)realloc(s->input, capacity);
    if (input == NULL)
      return -1;
    s->input = input;
    s->input_capacity = capacity;
  }

  size_t space = s->input_capacity - s->input_len;
  size_t size = stream->src_size < space ? stream->src_size : space;
  if (size > 0) {
    memcpy(s->input + s->input_len, stream->src_ptr, size);
    s->input_len += size;
    stream->src_ptr += size;
    stream->src_size -= size;
  }
  return 0;
}

/* Keeps only the match history once every decoded byte is flushed. */
static void lzfse_stream_slide_output(lzfse_stream_state *s) {
  if (s->output_len < s->output_capacity)
    return;
  size_t keep = LZFSE_STREAM_HISTORY;
  memmove(s->output, s->output + s->output_len - keep, keep);
  s->output_len = keep;
  s->output_flushed = keep;
}

lzfse_stream_status lzfse_stream_init(lzfse_stream *stream) {
  lzfse_stream_state *s =
      (lzfse_stream_state *)calloc(1, sizeof(lzfse_stream_state));
  if (s == NULL)
    return LZFSE_STREAM_STATUS_ERROR;

  s->input_capacity = LZFSE_STREAM_INITIAL_INPUT;
  s->input = (uint8_t *)malloc(s->input_capacity);
  s->output_capacity = LZFSE_STREAM_HISTORY + LZFSE_STREAM_OUTPUT_CHUNK;
  s->output = (uint8_t *)malloc(s->output_capacity);
  if (s->input == NULL || s->output == NULL) {
    free(s->input);
    free(s->output);
    free(s);
    return LZFSE_STREAM_STATUS_ERROR;
  }

  stream->state = s;
  return LZFSE_STREAM_STATUS_OK;
}

lzfse_stream_status lzfse_stream_process(lzfse_stream *stream, int finalize) {
  lzfse_stream_state *s = (lzfse_stream_state *)stream->state;
  if (s == NULL)
    return LZFSE_STREAM_STATUS_ERROR;

  while (1) {
    lzfse_stream_flush(stream, s);
    if (s->output_flushed < s->output_len)
      return LZFSE_STREAM_STATUS_OK; // dst is full

    if (s->finished)
      return LZFSE_STREAM_STATUS_END;

    if (lzfse_stream_absorb(stream, s) != 0)
      return LZFSE_STREAM_STATUS_ERROR;
    lzfse_stream_slide_output(s);

    lzfse_decoder_state *d = &s->decoder;
    d->src_begin = s->input;
    d->src = s->input + s->input_pos;
    d->src_end = s->input + s->input_len;
    d->dst_begin = s->output;
    d->dst = s->output + s->output_len;
    d->dst_end = s->output + s->output_capacity;

    size_t input_pos_before = s->input_pos;
    size_t output_len_before = s->output_len;

    int status = lzfse_decode(d);

    s->input_pos = (size_t)(d->src - s->input);
    s->output_len = (size_t)(d->dst - s->output);

    // LZVN reports DST_FULL when it stops on a partial opcode at the end of src.
    int made_progress = s->input_pos != input_pos_before ||
                        s->output_len != output_len_before;
    if (status == LZFSE_STATUS_DST_FULL && made_progress == 0 &&
        s->output_len < s->output_capacity)
      status = LZFSE_STATUS_SRC_EMPTY;

    switch (status) {
    case LZFSE_STATUS_OK:
      s->finished = 1;
      break;
    case LZFSE_STATUS_DST_FULL:
      break;
    case LZFSE_STATUS_SRC_EMPTY:
      if (s->output_flushed < s->output_len || stream->src_size > 0)
        break;
      return finalize ? LZFSE_STREAM_STATUS_ERROR : LZFSE_STREAM_STATUS_OK;
    default:
      return LZFSE_STREAM_STATUS_ERROR;
    }
  }
}

void lzfse_stream_destroy(lzfse_stream *stream) {
  lzfse_stream_state *s = (lzfse_stream_state *)stream->state;
  if (s == NULL)
    return;
  free(s->input);
  free(s->output);
  free(s);
  stream->state = NULL;
}
