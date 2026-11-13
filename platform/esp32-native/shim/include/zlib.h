/* The part of zlib's interface material_pack.c names. Only DNT3 packs (deflated textures) need it, and the
   asset sets the console reads are DNT6: every call fails, so a DNT3 pack is refused at open. */
#ifndef ESP_SHIM_ZLIB_H
#define ESP_SHIM_ZLIB_H
typedef unsigned char Bytef;
typedef unsigned int uInt;
typedef unsigned long uLong;
typedef void *voidpf;
typedef voidpf (*alloc_func)(voidpf opaque, uInt items, uInt size);
typedef void (*free_func)(voidpf opaque, voidpf address);
typedef struct {
    Bytef *next_in; uInt avail_in; uLong total_in;
    Bytef *next_out; uInt avail_out; uLong total_out;
    void *state; alloc_func zalloc; free_func zfree; voidpf opaque;
} z_stream;
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4
#define Z_DATA_ERROR (-3)
static inline int inflateInit(z_stream *s) { (void)s; return Z_DATA_ERROR; }
static inline int inflateReset(z_stream *s) { (void)s; return Z_DATA_ERROR; }
static inline int inflate(z_stream *s, int flush) { (void)s; (void)flush; return Z_DATA_ERROR; }
static inline int inflateEnd(z_stream *s) { (void)s; return Z_OK; }
#endif
