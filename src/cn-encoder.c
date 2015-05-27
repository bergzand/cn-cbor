#ifndef CN_ENCODER_C
#define CN_ENCODER_C

#ifdef  __cplusplus
extern "C" {
#endif
#ifdef EMACS_INDENTATION_HELPER
} /* Duh. */
#endif

#ifdef _MSC_VER
#include <WinSock2.h>
#else
#include <arpa/inet.h>
#endif
#include <string.h>
#ifndef _MSC_VER
#include <strings.h>
#endif
#include <stdbool.h>
#include <assert.h>

#include "cn-cbor/cn-cbor.h"
#include "cbor.h"

#define hton8p(p) (*(uint8_t*)(p))
#define hton16p(p) (htons(*(uint16_t*)(p)))
#define hton32p(p) (htonl(*(uint32_t*)(p)))
static uint64_t hton64p(const uint8_t *p) {
  /* TODO: does this work on both BE and LE systems? */
  uint64_t ret = hton32p(p);
  ret <<= 32;
  ret |= hton32p(p+4);
  return ret;
}

typedef struct _write_state
{
  uint8_t *buf;
  ssize_t offset;
  ssize_t size;
} cn_write_state;

#define ensure_writable(sz) if ((ws->offset<0) || (ws->offset + (sz) >= ws->size)) { \
  ws->offset = -1; \
  return; \
}

#define write_byte_and_data(b, data, sz) \
ws->buf[ws->offset++] = (b); \
memcpy(ws->buf+ws->offset, (data), (sz)); \
ws->offset += sz;

#define write_byte(b) \
ensure_writable(1); \
ws->buf[ws->offset++] = (b); \

static uint8_t _xlate[] = {
  IB_FALSE,    /* CN_CBOR_FALSE */
  IB_TRUE,     /* CN_CBOR_TRUE */
  IB_NIL,      /* CN_CBOR_NULL */
  IB_UNDEF,    /* CN_CBOR_UNDEF */
  IB_UNSIGNED, /* CN_CBOR_UINT */
  IB_NEGATIVE, /* CN_CBOR_INT */
  IB_BYTES,    /* CN_CBOR_BYTES */
  IB_TEXT,     /* CN_CBOR_TEXT */
  IB_BYTES,    /* CN_CBOR_BYTES_CHUNKED */
  IB_TEXT,     /* CN_CBOR_TEXT_CHUNKED */
  IB_ARRAY,    /* CN_CBOR_ARRAY */
  IB_MAP,      /* CN_CBOR_MAP */
  IB_TAG,      /* CN_CBOR_TAG */
  IB_PRIM,     /* CN_CBOR_SIMPLE */
  0xFF,        /* CN_CBOR_DOUBLE */
  0xFF         /* CN_CBOR_INVALID */
};

static inline bool is_indefinite(const cn_cbor *cb)
{
  return (cb->flags & CN_CBOR_FL_INDEF) != 0;
}

static void _write_positive(cn_write_state *ws, cn_cbor_type typ, uint64_t val) {
  uint8_t ib;

  assert((size_t)typ < sizeof(_xlate));

  ib = _xlate[typ];
  if (ib == 0xFF) {
    ws->offset = -1;
    return;
  }

  if (val < 24) {
    ensure_writable(1);
    write_byte(ib | val);
  } else if (val < 256) {
    ensure_writable(2);
    write_byte(ib | 24);
    write_byte((uint8_t)val);
  } else if (val < 65536) {
    uint16_t be16 = (uint16_t)val;
    ensure_writable(3);
    be16 = hton16p(&be16);
    write_byte_and_data(ib | 25, (const void*)&be16, 2);
  } else if (val < 0x100000000L) {
    uint32_t be32 = (uint32_t)val;
    ensure_writable(5);
    be32 = hton32p(&be32);
    write_byte_and_data(ib | 26, (const void*)&be32, 4);
  } else {
    uint64_t be64;
    ensure_writable(9);
    be64 = hton64p((const uint8_t*)&val);
    write_byte_and_data(ib | 27, (const void*)&be64, 8);
  }
}

static void _write_double(cn_write_state *ws, double val)
{
  uint64_t be64;
  /* Copy the same problematic implementation from the decoder. */
  union {
    double d;
    uint64_t u;
  } u64;
  /* TODO: cast double to float and back, and see if it changes.
     See cabo's ruby code for more:
     https://github.com/cabo/cbor-ruby/blob/master/ext/cbor/packer.h */

  ensure_writable(9);
  u64.d = val;
  be64 = hton64p((const uint8_t*)&u64.u);

  write_byte_and_data(IB_PRIM | 27, (const void*)&be64, 8);
}

// TODO: make public?
typedef void (*cn_visit_func)(const cn_cbor *cb, int depth, void *context);
static void _visit(const cn_cbor *cb,
                   cn_visit_func visitor,
                   cn_visit_func breaker,
                   void *context)
{
    const cn_cbor *p = cb;
    int depth = 0;
    while (p)
    {
visit:
      visitor(p, depth, context);
      if (p->first_child) {
        p = p->first_child;
        depth++;
      } else{
        // Empty indefinite
        if (is_indefinite(p)) {
          breaker(p->parent, depth, context);
        }
        if (p->next) {
          p = p->next;
        } else {
          while (p->parent) {
            depth--;
            if (is_indefinite(p->parent)) {
              breaker(p->parent, depth, context);
            }
            if (p->parent->next) {
              p = p->parent->next;
              goto visit;
            }
            p = p->parent;
          }
          return;
        }
      }
    }
}

#define CHECK(st) (st); \
if (ws->offset < 0) { return; }

void _encoder_visitor(const cn_cbor *cb, int depth, void *context)
{
  cn_write_state *ws = context;
  UNUSED_PARAM(depth);

  switch (cb->type) {
  case CN_CBOR_ARRAY:
    if (is_indefinite(cb)) {
      write_byte(IB_ARRAY | AI_INDEF);
    } else {
      CHECK(_write_positive(ws, CN_CBOR_ARRAY, cb->length));
    }
    break;
  case CN_CBOR_MAP:
    if (is_indefinite(cb)) {
      write_byte(IB_MAP | AI_INDEF);
    } else {
      CHECK(_write_positive(ws, CN_CBOR_MAP, cb->length/2));
    }
    break;
  case CN_CBOR_BYTES_CHUNKED:
  case CN_CBOR_TEXT_CHUNKED:
    write_byte(_xlate[cb->type] | AI_INDEF);
    break;

  case CN_CBOR_TEXT:
  case CN_CBOR_BYTES:
    CHECK(_write_positive(ws, cb->type, cb->length));
    ensure_writable(cb->length);
    memcpy(ws->buf+ws->offset, cb->v.str, cb->length);
    ws->offset += cb->length;
    break;

  case CN_CBOR_FALSE:
  case CN_CBOR_TRUE:
  case CN_CBOR_NULL:
  case CN_CBOR_UNDEF:
    write_byte(_xlate[cb->type]);
    break;

  case CN_CBOR_TAG:
  case CN_CBOR_UINT:
  case CN_CBOR_SIMPLE:
    CHECK(_write_positive(ws, cb->type, cb->v.uint));
    break;

  case CN_CBOR_INT:
    assert(cb->v.sint < 0);
    CHECK(_write_positive(ws, CN_CBOR_INT, ~(cb->v.sint)));
    break;

  case CN_CBOR_DOUBLE:
    CHECK(_write_double(ws, cb->v.dbl));
    break;

  case CN_CBOR_INVALID:
    ws->offset = -1;
    break;
  }
}

void _encoder_breaker(const cn_cbor *cb, int depth, void *context)
{
  cn_write_state *ws = context;
  UNUSED_PARAM(cb);
  UNUSED_PARAM(depth);
  write_byte(IB_BREAK);
}

ssize_t cbor_encoder_write(uint8_t *buf,
                           size_t buf_offset,
                           size_t buf_size,
                           const cn_cbor *cb)
{
  cn_write_state ws = { buf, buf_offset, buf_size };
  _visit(cb, _encoder_visitor, _encoder_breaker, &ws);
  if (ws.offset < 0) { return -1; }
  return ws.offset - buf_offset;
}

#ifdef  __cplusplus
}
#endif

#endif  /* CN_CBOR_C */
