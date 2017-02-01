// ============================================================================
//
// = LIBRARY
//    ULib - c++ library
//
// = FILENAME
//    http2.cpp - HTTP/2 utility 
//
// = AUTHOR
//    Stefano Casazza
//
// ============================================================================

#include <ulib/utility/http2.h>
#include <ulib/utility/hpack_huffman_table.h> // huff_sym_table, huff_decode_table

int                           UHTTP2::nerror;
int                           UHTTP2::hpack_errno;
bool                          UHTTP2::bnghttp2;
bool                          UHTTP2::bcontinue100;
bool                          UHTTP2::bsetting_ack;
bool                          UHTTP2::bsetting_send;
bool                          UHTTP2::priority_exclusive;
uint8_t                       UHTTP2::priority_weight;     // 0 if not set
uint32_t                      UHTTP2::priority_dependency; // 0 if not set
uint32_t                      UHTTP2::hash_static_table[61];
uint32_t                      UHTTP2::wait_for_continuation;
UHTTP2::Stream*               UHTTP2::pStream;
UHTTP2::Stream*               UHTTP2::pStreamEnd;
UHTTP2::FrameHeader           UHTTP2::frame;
UHTTP2::Connection*           UHTTP2::vConnection;
UHTTP2::Connection*           UHTTP2::pConnection;
UHTTP2::HpackHeaderTableEntry UHTTP2::hpack_static_table[61];

#ifdef DEBUG
#  ifndef UINT16_MAX
#  define UINT16_MAX 65535
#  endif

bool               UHTTP2::bhash;
bool               UHTTP2::btest;
bool               UHTTP2::bdecodeHeadersDebug;
unsigned char*     UHTTP2::index_ptr;
UHTTP2::HpackError UHTTP2::hpack_error[8];
#endif

#define HTTP2_CONNECTION_UPGRADE          \
   "HTTP/1.1 101 Switching Protocols\r\n" \
   "Connection: Upgrade\r\n"              \
   "Upgrade: h2c\r\n\r\n"

#define HTTP2_MAX_WINDOW_SIZE    (2147483647-1)
#define HTTP2_DEFAULT_WINDOW_SIZE 65535 

// =====================================================
//            SETTINGS FRAME
// =====================================================
// frame size = 12 (2*6)
// settings frame
// no flags
// stream id = 0
// =====================================================
// max_concurrent_streams = HTTP2_MAX_CONCURRENT_STREAMS
// max_frame_size = 16777215
// =====================================================
// 9 + 12 = 21 bytes
// =====================================================

#define HTTP2_SETTINGS_BIN \
   "\x00\x00\x06"          \
   "\x04"                  \
   "\x00"                  \
   "\x00\x00\x00\x00"      \
   "\x00\x03" "\x00\x00\x00\x80"
// "\x00\x05" "\x00\xff\xff\xff" (h2spec(4.2) fail with this..????)

// ================================
//       SETTINGS ACK FRAME
// ================================
// frame size = 0
// settings frame
// flags ACK
// stream id = 0
// ================================
// (9 bytes)
// ================================
#define HTTP2_SETTINGS_ACK \
   "\x00\x00\x00"          \
   "\x04"                  \
   "\x01"                  \
   "\x00\x00\x00\x00"

#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif

const UHTTP2::Settings UHTTP2::settings = {
   /* header_table_size = */ 4096,
   /* enable_push = */ 1,
   /* max_concurrent_streams = */ UINT32_MAX,
   /* initial_window_size = (HTTP2_DEFAULT_WINDOW_SIZE) */ 65535,
   /* max_frame_size = */ 16384,
   /* max_header_list_size */ UINT32_MAX
};

UHTTP2::Connection::Connection() : itable(53, setIndexStaticTable)
#ifdef DEBUG
, dtable(53, setIndexStaticTable)
#endif
{
   U_TRACE_REGISTER_OBJECT(0, Connection, "", 0)

   reset();

   state = CONN_STATE_IDLE; 

   (void) memset(&idyntbl, 0, sizeof(HpackDynamicTable));
   (void) memset(&odyntbl, 0, sizeof(HpackDynamicTable));

   idyntbl.hpack_capacity     =
   idyntbl.hpack_max_capacity =
   odyntbl.hpack_capacity     =
   odyntbl.hpack_max_capacity = 4096;

   for (uint32_t i = 0; i < HTTP2_MAX_CONCURRENT_STREAMS; ++i) (void) U_SYSCALL(memset, "%p,%d,%u", &(streams[i].id), 0, sizeof(uint32_t) * 3);

#ifdef DEBUG
   (void) memset(&ddyntbl, 0, sizeof(HpackDynamicTable));

   ddyntbl.hpack_capacity     =
   ddyntbl.hpack_max_capacity = 4096;
#endif
}

#ifdef DEBUG
#define U_HTTP2_HPACK_ERROR_SET_ENTRY(i,s,v,d) \
   hpack_error[i].str   = s; \
   hpack_error[i].value = v; \
   hpack_error[i].desc  = d
#endif

void UHTTP2::ctor()
{
   U_TRACE_NO_PARAM(0+256, "UHTTP2::ctor()")

   UString::str_allocate(STR_ALLOCATE_HTTP2);

   hpack_static_table[ 0].name  = UString::str_authority;
    hash_static_table[ 0]       = UString::str_authority->hashIgnoreCase();
   hpack_static_table[ 1].name  = UString::str_method;
    hash_static_table[ 1]       = UString::str_method->hashIgnoreCase();
   hpack_static_table[ 1].value = UString::str_method_get;
   hpack_static_table[ 2].name  = UString::str_method;
   hpack_static_table[ 2].value = UString::str_method_post;
   hpack_static_table[ 3].name  = UString::str_path;
    hash_static_table[ 3]       = UString::str_path->hashIgnoreCase();
   hpack_static_table[ 3].value = UString::str_path_root;
   hpack_static_table[ 4].name  = UString::str_path;
   hpack_static_table[ 4].value = UString::str_path_index;
   hpack_static_table[ 5].name  = UString::str_scheme;
    hash_static_table[ 5]       = UString::str_scheme->hashIgnoreCase();
   hpack_static_table[ 5].value = UString::str_http;
   hpack_static_table[ 6].name  = UString::str_scheme;
   hpack_static_table[ 6].value = UString::str_scheme_https;
   hpack_static_table[ 7].name  = UString::str_status;
    hash_static_table[ 7]       = UString::str_status->hashIgnoreCase();
   hpack_static_table[ 7].value = UString::str_status_200;
   hpack_static_table[ 8].name  = UString::str_status;
   hpack_static_table[ 8].value = UString::str_status_204;
   hpack_static_table[ 9].name  = UString::str_status;
   hpack_static_table[ 9].value = UString::str_status_206;
   hpack_static_table[10].name  = UString::str_status;
   hpack_static_table[10].value = UString::str_status_304;
   hpack_static_table[11].name  = UString::str_status;
   hpack_static_table[11].value = UString::str_status_400;
   hpack_static_table[12].name  = UString::str_status;
   hpack_static_table[12].value = UString::str_status_404;
   hpack_static_table[13].name  = UString::str_status;
   hpack_static_table[13].value = UString::str_status_500;
   hpack_static_table[14].name  = UString::str_accept_charset;
    hash_static_table[14]       = UString::str_accept_charset->hashIgnoreCase();
   hpack_static_table[15].name  = UString::str_accept_encoding;
    hash_static_table[15]       = UString::str_accept_encoding->hashIgnoreCase();
   hpack_static_table[15].value = UString::str_accept_encoding_value;
   hpack_static_table[16].name  = UString::str_accept_language;
    hash_static_table[16]       = UString::str_accept_language->hashIgnoreCase();
   hpack_static_table[17].name  = UString::str_accept_ranges;
    hash_static_table[17]       = UString::str_accept_ranges->hashIgnoreCase();
   hpack_static_table[18].name  = UString::str_accept;
    hash_static_table[18]       = UString::str_accept->hashIgnoreCase();
   hpack_static_table[19].name  = UString::str_access_control_allow_origin;
    hash_static_table[19]       = UString::str_access_control_allow_origin->hashIgnoreCase();
   hpack_static_table[20].name  = UString::str_age;
    hash_static_table[20]       = UString::str_age->hashIgnoreCase();
   hpack_static_table[21].name  = UString::str_allow;
    hash_static_table[21]       = UString::str_allow->hashIgnoreCase();
   hpack_static_table[22].name  = UString::str_authorization;
    hash_static_table[22]       = UString::str_authorization->hashIgnoreCase();
   hpack_static_table[23].name  = UString::str_cache_control;
    hash_static_table[23]       = UString::str_cache_control->hashIgnoreCase();
   hpack_static_table[24].name  = UString::str_content_disposition;
    hash_static_table[24]       = UString::str_content_disposition->hashIgnoreCase();
   hpack_static_table[25].name  = UString::str_content_encoding;
    hash_static_table[25]       = UString::str_content_encoding->hashIgnoreCase();
   hpack_static_table[26].name  = UString::str_content_language;
    hash_static_table[26]       = UString::str_content_language->hashIgnoreCase();
   hpack_static_table[27].name  = UString::str_content_length;
    hash_static_table[27]       = UString::str_content_length->hashIgnoreCase();
   hpack_static_table[28].name  = UString::str_content_location;
    hash_static_table[28]       = UString::str_content_location->hashIgnoreCase();
   hpack_static_table[29].name  = UString::str_content_range;
    hash_static_table[29]       = UString::str_content_range->hashIgnoreCase();
   hpack_static_table[30].name  = UString::str_content_type;
    hash_static_table[30]       = UString::str_content_type->hashIgnoreCase();
   hpack_static_table[31].name  = UString::str_cookie;
    hash_static_table[31]       = UString::str_cookie->hashIgnoreCase();
   hpack_static_table[32].name  = UString::str_date;
    hash_static_table[32]       = UString::str_date->hashIgnoreCase();
   hpack_static_table[33].name  = UString::str_etag;
    hash_static_table[33]       = UString::str_etag->hashIgnoreCase();
   hpack_static_table[34].name  = UString::str_expect;
    hash_static_table[34]       = UString::str_expect->hashIgnoreCase();
   hpack_static_table[35].name  = UString::str_expires;
    hash_static_table[35]       = UString::str_expires->hashIgnoreCase();
   hpack_static_table[36].name  = UString::str_from;
    hash_static_table[36]       = UString::str_from->hashIgnoreCase();
   hpack_static_table[37].name  = UString::str_host;
    hash_static_table[37]       = UString::str_host->hashIgnoreCase();
   hpack_static_table[38].name  = UString::str_if_match;
    hash_static_table[38]       = UString::str_if_match->hashIgnoreCase();
   hpack_static_table[39].name  = UString::str_if_modified_since;
    hash_static_table[39]       = UString::str_if_modified_since->hashIgnoreCase();
   hpack_static_table[40].name  = UString::str_if_none_match;
    hash_static_table[40]       = UString::str_if_none_match->hashIgnoreCase();
   hpack_static_table[41].name  = UString::str_if_range;
    hash_static_table[41]       = UString::str_if_range->hashIgnoreCase();
   hpack_static_table[42].name  = UString::str_if_unmodified_since;
    hash_static_table[42]       = UString::str_if_unmodified_since->hashIgnoreCase();
   hpack_static_table[43].name  = UString::str_last_modified;
    hash_static_table[43]       = UString::str_last_modified->hashIgnoreCase();
   hpack_static_table[44].name  = UString::str_link;
    hash_static_table[44]       = UString::str_link->hashIgnoreCase();
   hpack_static_table[45].name  = UString::str_location;
    hash_static_table[45]       = UString::str_location->hashIgnoreCase();
   hpack_static_table[46].name  = UString::str_max_forwards;
    hash_static_table[46]       = UString::str_max_forwards->hashIgnoreCase();
   hpack_static_table[47].name  = UString::str_proxy_authenticate;
    hash_static_table[47]       = UString::str_proxy_authenticate->hashIgnoreCase();
   hpack_static_table[48].name  = UString::str_proxy_authorization;
    hash_static_table[48]       = UString::str_proxy_authorization->hashIgnoreCase();
   hpack_static_table[49].name  = UString::str_range;
    hash_static_table[49]       = UString::str_range->hashIgnoreCase();
   hpack_static_table[50].name  = UString::str_referer;
    hash_static_table[50]       = UString::str_referer->hashIgnoreCase();
   hpack_static_table[51].name  = UString::str_refresh;
    hash_static_table[51]       = UString::str_refresh->hashIgnoreCase();
   hpack_static_table[52].name  = UString::str_retry_after;
    hash_static_table[52]       = UString::str_retry_after->hashIgnoreCase();
   hpack_static_table[53].name  = UString::str_server;
    hash_static_table[53]       = UString::str_server->hashIgnoreCase();
   hpack_static_table[54].name  = UString::str_set_cookie;
    hash_static_table[54]       = UString::str_set_cookie->hashIgnoreCase();
   hpack_static_table[55].name  = UString::str_strict_transport_security;
    hash_static_table[55]       = UString::str_strict_transport_security->hashIgnoreCase();
   hpack_static_table[56].name  = UString::str_transfer_encoding;
    hash_static_table[56]       = UString::str_transfer_encoding->hashIgnoreCase();
   hpack_static_table[57].name  = UString::str_user_agent;
    hash_static_table[57]       = UString::str_user_agent->hashIgnoreCase();
   hpack_static_table[58].name  = UString::str_vary;
    hash_static_table[58]       = UString::str_vary->hashIgnoreCase();
   hpack_static_table[59].name  = UString::str_via;
    hash_static_table[59]       = UString::str_via->hashIgnoreCase();
   hpack_static_table[60].name  = UString::str_www_authenticate;
    hash_static_table[60]       = UString::str_www_authenticate->hashIgnoreCase();

#ifdef DEBUG
   if (btest)
      {
      U_http_version = '2';

      UNotifier::max_connection = 1;

      U_HTTP2_HPACK_ERROR_SET_ENTRY( 0, "BUF", -2,  "buffer overflow");       // The decoding buffer ends before the decoded HPACK block, reading
                                                                              // further would result in a buffer overflow. This error is turned
                                                                              // into a BLK result when decoding is partial
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 1, "INT", -3,  "integer overflow");      // Decoding of an integer gives a value too large
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 2, "IDX", -4,  "invalid index");         // The decoded or specified index is out of range
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 3, "LEN", -5,  "invalid length");        // An invalid length may refer to header fields with an empty name
                                                                              // or a table size that exceeds the maximum. Anything that doesn't
                                                                              // meet a length requirement
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 4, "HUF", -6,  "invalid Huffman code");  // A decoder decoded an invalid Huffman sequence
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 5, "CHR", -7,  "invalid character");     // A invalid header name or value character was coded
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 6, "UPD", -8,  "spurious update");       // A table update occurred at a wrong time or with a wrong size
      U_HTTP2_HPACK_ERROR_SET_ENTRY( 7, "RSZ", -9,  "missing resize update"); // A table update was expected after a table resize but didn't occur

      Connection::preallocate(1);

      (void) initRequest();
      }
#endif
}

#ifdef DEBUG
#undef U_HTTP2_HPACK_ERROR_SET_ENTRY
#endif

void UHTTP2::updateSetting(unsigned char* ptr, uint32_t len)
{
   U_TRACE(0, "UHTTP2::updateSetting(%#.*S,%u)", len, ptr, len)

   U_INTERNAL_ASSERT_MAJOR(len, 0)
   U_INTERNAL_ASSERT_POINTER(pConnection)

   for (; len >= 6; len -= 6, ptr += 6)
      {
      uint32_t value = ntohl(*(uint32_t*)(ptr+2));

      switch (ntohs(*(uint16_t*)ptr))
         {
         case ENABLE_PUSH:
            {
            if (value > 1)
               {
               nerror = PROTOCOL_ERROR;

               return;
               }

            pConnection->peer_settings.enable_push = value;
            }
         break;

         case INITIAL_WINDOW_SIZE:
            {
            if (value > HTTP2_MAX_WINDOW_SIZE)
               {
               nerror = FLOW_CONTROL_ERROR;

               return;
               }

            pConnection->out_window                        =
            pConnection->peer_settings.initial_window_size = value;
            }
         break;

         case MAX_FRAME_SIZE:
            {
            if (value < 16384 ||
                value > 16777215)
               {
               nerror = PROTOCOL_ERROR;

               return;
               }

            pConnection->peer_settings.max_frame_size = value;
            }
         break;

         case MAX_CONCURRENT_STREAMS:                        pConnection->peer_settings.max_concurrent_streams = value;  break;
         case MAX_HEADER_LIST_SIZE:                          pConnection->peer_settings.max_header_list_size   = value;  break;
         case HEADER_TABLE_SIZE: setHpackInputDynTblCapacity(pConnection->peer_settings.header_table_size      = value); break;

         default: break; // ignore unknown (5.5)
         }
      }

   if (len) nerror = FRAME_SIZE_ERROR;

   U_INTERNAL_DUMP("header_table_size = %u enable_push = %u max_concurrent_streams = %u initial_window_size = %u max_frame_size = %u max_header_list_size = %u",
                     pConnection->peer_settings.header_table_size,   pConnection->peer_settings.enable_push,    pConnection->peer_settings.max_concurrent_streams,
                     pConnection->peer_settings.initial_window_size, pConnection->peer_settings.max_frame_size, pConnection->peer_settings.max_header_list_size)
}

unsigned char* UHTTP2::hpackDecodeInt(unsigned char* src, unsigned char* src_end, int32_t& value, uint8_t prefix_max)
{
   U_TRACE(0, "UHTTP2::hpackDecodeInt(%p,%p,%p,%u)", src, src_end, &value, prefix_max)

   uint8_t mult;

#ifdef DEBUG
   if (src == src_end) goto overflow;
#endif

   if ((value = (uint8_t)*src++ & prefix_max) == prefix_max)
      {
      mult = 0;

loop:
#  ifdef DEBUG
      if (src == src_end)
         {
overflow:
         hpack_errno = -2; // The decoding buffer ends before the decoded HPACK block

         goto err;
         }
#  endif

      value += (*src & 0x7f) << mult;

#  ifdef DEBUG
      if (value > UINT16_MAX) goto too_large;
#  endif

      if ((*src++ & 0x80) == 0) goto end;

      mult += 7;

#  ifdef DEBUG
      if (mult >= 32) // we only allow at most 4 octets (excluding prefix) to be used as int (== 2**(4*7) == 2**28)
         {
too_large:
         hpack_errno = -3; // Decoding of an integer gives a value too large

         goto err;
         }
#  endif

      goto loop;

err:  value = -1;
      }

end:
   U_INTERNAL_DUMP("value = %d hpack_errno = %d", value, hpack_errno)

   U_RETURN_POINTER(src, unsigned char);
}

unsigned char* UHTTP2::hpackDecodeString(unsigned char* src, unsigned char* src_end, UString& value)
{
   U_TRACE(0, "UHTTP2::hpackDecodeString(%p,%p,%p)", src, src_end, &value)

   int32_t len;
   bool is_huffman = ((*src & 0x80) != 0);

   U_INTERNAL_DUMP("is_huffman = %b", is_huffman)

   src = hpackDecodeInt(src, src_end, len, (1<<7)-1);

   U_INTERNAL_DUMP("len = %d", len)

   if (len <= 0)
      {
err:  value.clear();

      U_RETURN_POINTER(src, unsigned char);
      }

#ifdef DEBUG
   if (src == src_end)
      {
      hpack_errno = -2; // // The decoding buffer ends before the decoded HPACK block

      goto err;
      }
#endif

   src_end = src+len;

   if (is_huffman == false) (void) value.replace((const char*)src, len);
   else
      {
      uint8_t state = 0;
      const HuffDecode* entry;
      UString result(len * 2); // max compression ratio is >= 0.5
      char* dst = result.data();

loop: entry = huff_decode_table[state] + (*src >> 4);

      if ((entry->flags & HUFF_FAIL) != 0)
         {
#     ifdef DEBUG
         hpack_errno = -6; // A decoder decoded an invalid Huffman sequence
#     endif

         goto err;
         }

      if ((entry->flags & HUFF_SYM) != 0) *dst++ = entry->sym;

      entry = huff_decode_table[entry->state] + (*src & 0x0f);

      if ((entry->flags & HUFF_FAIL) != 0)
         {
#     ifdef DEBUG
         hpack_errno = -6; // A decoder decoded an invalid Huffman sequence
#     endif

         goto err;
         }

      if ((entry->flags & HUFF_SYM) != 0) *dst++ = entry->sym;

      if (++src < src_end)
         {
         state = entry->state;

         goto loop;
         }

      U_INTERNAL_DUMP("maybe_eos = %b entry->state = %d", (entry->flags & HUFF_ACCEPTED) != 0, entry->state)

      if ((entry->flags & HUFF_ACCEPTED) == 0)
         {
#     ifdef DEBUG
         hpack_errno = (entry->state == 28 ? -7   // A invalid header name or value character was coded
                                           : -6); // A decoder decoded an invalid Huffman sequence
#     endif

         goto err;
         }

      result.size_adjust(dst);

      value = result;
      }

   U_INTERNAL_DUMP("value = %V", value.rep)

   U_RETURN_POINTER(src_end, unsigned char);
}

unsigned char* UHTTP2::hpackEncodeString(unsigned char* dst, const char* src, uint32_t len, bool bhuffman)
{
   U_TRACE(0+256, "UHTTP2::hpackEncodeString(%p,%.*S,%u,%b)", dst, len, src, len, bhuffman)

   U_INTERNAL_ASSERT_MAJOR(len, 0)

#ifdef DEBUG
   if (isHeaderValue(src, len) == false)
      {
      U_INTERNAL_DUMP("u_isText() = %b u_isUTF8() = %b u_isUTF16() = %b", u_isText( (const unsigned char*)src, len),
                                                                          u_isUTF8( (const unsigned char*)src, len),
                                                                          u_isUTF16((const unsigned char*)src, len))

      hpack_errno = -7; // A invalid header name or value character was coded

      U_RETURN_POINTER(dst, unsigned char);
      }
   
// unsigned char* dst0 = dst;
#endif

   if (bhuffman == false) // encode as-is
      {
      dst = hpackEncodeInt(dst, len, (1<<7)-1, 0x00);

      U_MEMCPY(dst, src, len);

      U_RETURN_POINTER(dst+len, unsigned char);
      }

   uint64_t bits = 0;
   const HuffSym* sym;
   const char* ptr = src;
   uint32_t dst_len = 0, sz = 0;
   const char* src_end = src + len;

   if (len < ((1<<7)-1))
      {
      unsigned char* _dst  =   dst;
      unsigned char* start = ++dst;

      do {
         sym = huff_sym_table + *(unsigned char*)ptr;

         bits = (bits << sym->nbits) | sym->code;

         dst_len += sym->nbits;
              sz += sym->nbits;

         while (sz >= 8)
            {
            sz -= 8;

            *dst++ = (uint8_t)(bits >> sz);
            }
         }
      while (++ptr < src_end);

      _dst = setHpackEncodeStringLen(_dst, dst_len);

      U_INTERNAL_ASSERT_EQUALS(_dst, start)
      }
   else
      {
      do {
         sym = huff_sym_table + *(unsigned char*)ptr;

         dst_len += sym->nbits;
         }
      while (++ptr < src_end);

      dst = setHpackEncodeStringLen(dst, dst_len);

      ptr = src;

      do {
         sym = huff_sym_table + *(unsigned char*)ptr;

         bits = (bits << sym->nbits) | sym->code;

         sz += sym->nbits;

         while (sz >= 8)
            {
            sz -= 8;

            *dst++ = (uint8_t)(bits >> sz);
            }
         }
      while (++ptr < src_end);
      }

   U_INTERNAL_DUMP("sz = %u dst_len = %u", sz, dst_len)

   U_INTERNAL_ASSERT_MINOR(sz, 8)

   if (sz > 0)
      {
      sz = 8 - sz;
      bits <<= sz;
      bits |= (1 << sz) - 1;

      *dst++ = (uint8_t)bits;
      }

/*
#ifdef DEBUG
   UString value;
   (void) hpackDecodeString(dst0, dst, value);
   U_INTERNAL_ASSERT(value.equal(src, len))
#endif
*/

   U_RETURN_POINTER(dst, unsigned char);
}

void UHTTP2::addHpackDynTblEntry(HpackDynamicTable* dyntbl, const UString& name, const UString& value)
{
   U_TRACE(1, "UHTTP2::addHpackDynTblEntry(%p,%V,%V)", dyntbl, name.rep, value.rep)

   U_INTERNAL_ASSERT(name)

   uint32_t size_add = name.size() + value.size() + HTTP2_HEADER_TABLE_ENTRY_SIZE_OFFSET;

   U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u size_add = %u hpack_capacity = %u hpack_max_capacity = %u",
                     dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, size_add, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)

   // adjust the size

   while (dyntbl->num_entries != 0 &&
          (dyntbl->hpack_size + size_add) > dyntbl->hpack_capacity)
      {
      evictHpackDynTblEntry(dyntbl);
      }

   if (dyntbl->num_entries == 0)
      {
      U_INTERNAL_ASSERT_EQUALS(dyntbl->hpack_size, 0)

      if (size_add > dyntbl->hpack_capacity) return;
      }

   dyntbl->hpack_size += size_add;

   // if full grow the entries

   if (dyntbl->num_entries == dyntbl->entry_capacity)
      {
      uint32_t new_capacity = dyntbl->num_entries * 2;

      if (new_capacity < 16) new_capacity = 16;

      HpackHeaderTableEntry* new_entries = (HpackHeaderTableEntry*) UMemoryPool::_malloc(&new_capacity, sizeof(HpackHeaderTableEntry));

      if (dyntbl->num_entries != 0)
         {
         uint32_t src_index = dyntbl->entry_start_index,
                  dst_index = 0;

         do {
            new_entries[dst_index] = dyntbl->entries[src_index];

            if (++src_index == dyntbl->entry_capacity) src_index = 0;
            }
         while (++dst_index != dyntbl->num_entries);
         }

      (void) U_SYSCALL(memset, "%p,%d,%u", new_entries + dyntbl->num_entries, 0, sizeof(HpackHeaderTableEntry) * (new_capacity - dyntbl->num_entries));

      if (dyntbl->entry_capacity) UMemoryPool::_free(dyntbl->entries, dyntbl->entry_capacity, sizeof(HpackHeaderTableEntry));

      dyntbl->entries           = new_entries;
      dyntbl->entry_capacity    = new_capacity;
      dyntbl->entry_start_index = 0;
      }

   dyntbl->num_entries++;

   U_INTERNAL_ASSERT_MINOR(dyntbl->num_entries, 65536)

   dyntbl->entry_start_index = (dyntbl->entry_start_index - 1 + dyntbl->entry_capacity) % dyntbl->entry_capacity;

   UHTTP2::HpackHeaderTableEntry* entry = dyntbl->entries + dyntbl->entry_start_index;

   U_NEW(UString, entry->name,  UString(name));
   U_NEW(UString, entry->value, UString(value));

   U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u hpack_capacity = %u hpack_max_capacity = %u",
                     dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)
}

void UHTTP2::decodeHeaders(UHashMap<UString>* table, HpackDynamicTable* dyntbl, unsigned char* ptr, unsigned char* endptr)
{
   U_TRACE(0+256, "UHTTP2::decodeHeaders(%p,%p,%p,%p)", table, dyntbl, ptr, endptr)

   U_INTERNAL_ASSERT_EQUALS(U_http_version, '2')

   static const int dispatch_table[HTTP2_HEADER_TABLE_OFFSET] = {
      0,/* 0 */
      (int)((char*)&&case_1-(char*)&&cdefault),   /* 1 :authority */
      (int)((char*)&&case_2_3-(char*)&&cdefault), /* 2 :method */
      (int)((char*)&&case_2_3-(char*)&&cdefault), /* 3 :method */
      (int)((char*)&&case_4_5-(char*)&&cdefault), /* 4 :path */
      (int)((char*)&&case_4_5-(char*)&&cdefault), /* 5 :path */
      (int)((char*)&&case_6-(char*)&&cdefault),   /* 6 :scheme */
      (int)((char*)&&case_7-(char*)&&cdefault),   /* 7 :scheme */
      (int)((char*)&&case_8-(char*)&&cdefault),   /* 8 :status 200 */
      (int)((char*)&&case_9-(char*)&&cdefault),   /* 9 :status 204 */
      (int)((char*)&&case_10-(char*)&&cdefault),  /* 10 :status 206 */
      (int)((char*)&&case_11-(char*)&&cdefault),  /* 11 :status 304 */
      (int)((char*)&&case_12-(char*)&&cdefault),  /* 12 :status 400 */
      (int)((char*)&&case_13-(char*)&&cdefault),  /* 13 :status 404 */
      (int)((char*)&&case_14-(char*)&&cdefault),  /* 14 :status 500 */
      0,/* 15 accept-charset */
      (int)((char*)&&case_16-(char*)&&cdefault),  /* 16 accept-encoding */
      (int)((char*)&&case_17-(char*)&&cdefault),  /* 17 accept-language */
      0,/* 18 accept-ranges */
      (int)((char*)&&case_19-(char*)&&cdefault),  /* 19 accept */
      0,/* 20 access-control-allow-origin */
      0,/* 21 age */
      0,/* 22 allow */
      0,/* 23 authorization */
      0,/* 24 cache-control */
      0,/* 25 content-disposition */
      0,/* 26 content-encoding */
      0,/* 27 content-language */
      (int)((char*)&&case_28-(char*)&&cdefault), /* 28 content-length */
      0,/* 29 content-location */
      0,/* 30 content-range */
      (int)((char*)&&case_31-(char*)&&cdefault), /* 31 content-type */
      (int)((char*)&&case_32-(char*)&&cdefault), /* 32 cookie */
      0,/* 33 date */
      0,/* 34 etag */
      (int)((char*)&&case_35-(char*)&&cdefault), /* 35 expect */
      0,/* 36 expires */
      0,/* 37 from */
      (int)((char*)&&case_38-(char*)&&cdefault), /* 38 host */
      0,/* 39 if-match */
      (int)((char*)&&case_40-(char*)&&cdefault), /* 40 if-modified-since */
      0,/* 41 if-none-match */
      0,/* 42 if-range */
      0,/* 43 if-unmodified-since */
      0,/* 44 last-modified */
      0,/* 45 link*/
      0,/* 46 location */
      0,/* 47 max-forwards*/
      0,/* 48 proxy-authenticate */
      0,/* 49 proxy-authorization */
      (int)((char*)&&case_50-(char*)&&cdefault), /* 50 range */
      (int)((char*)&&case_51-(char*)&&cdefault), /* 51 referer */
      0,/* 52 refresh */
      0,/* 53 retry-after */
      0,/* 54 server */
      0,/* 55 set-cookie */
      0,/* 56 strict-transport-security */
      (int)((char*)&&case_57-(char*)&&cdefault), /* 57 transfer-encoding */
      (int)((char*)&&case_58-(char*)&&cdefault), /* 58 user-agent */
      0,/* 59 vary */
      0,/* 60 via */
      0 /* 61 www-authenticate */
   };

   enum PseudoHeaderMask {
      METHOD = 0x01,
      SCHEME = 0x02,
      PATH   = 0x04
   };

   int32_t index;
   UString name, value;
   uint32_t pseudo_header_mask = 0;
   UHTTP2::HpackHeaderTableEntry* entry;
   bool bvalue_is_indexed = false, binsert_dynamic_table = false, bregular = false;

#ifdef DEBUG
   uint32_t upd = 0; // No more than two updates can occur in a block
#endif

   U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u hpack_capacity = %u hpack_max_capacity = %u",
                     dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)

   while (ptr < endptr)
      {
      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(binsert_dynamic_table, false)

#  ifdef DEBUG
       name.clear();
      value.clear();
#  endif

      index = *ptr;

      U_INTERNAL_DUMP("index = (\\%o %u 0x%X) %C %B", index, index, index, index, index)

      U_INTERNAL_DUMP("index & 0x80 = 0x%x", index & 0x80)

      if ((index & 0x80) == 0x80) // (128: 10000000) Section 6.1: Indexed Header Field Representation
         {
         bvalue_is_indexed = true;

         ptr = hpackDecodeInt(ptr, endptr, index, (1<<7)-1); // (127: 01111111)

#     ifdef DEBUG
         if (index == 0) hpack_errno = -4; // The decoded or specified index is out of range

         if (isHpackError(index)) return;
#     endif

         goto check2;
         }

      U_INTERNAL_DUMP("index & 0x40 = 0x%x", index & 0x40)

      if ((index & 0x40) == 0x40) // (64: 01000000) Section 6.2.1:  Literal Header Field with Incremental Indexing
         {
         binsert_dynamic_table = true; // incremental indexing implicitly adds a new entry into the dynamic table

         ptr = hpackDecodeInt(ptr, endptr, index, (1<<6)-1); // (63: 00111111)

#     ifdef DEBUG
         if (isHpackError(index)) return;
#     endif

         goto check1;
         }

      U_INTERNAL_DUMP("index & 0x20 = 0x%x", index & 0x20)

      if ((index & 0x20) == 0x20) // (32: 00100000) Section 6.3: Dynamic Table Size Update
         {
#     ifdef DEBUG
         if (index_ptr || // Table size update not at the begining of an HPACK block
             ++upd > 2)   // No more than two updates can occur in a block
            {
            goto upd_err;
            }
#     endif

         ptr = hpackDecodeInt(ptr, endptr, index, (1<<5)-1); // (31: 00011111)

#     ifdef DEBUG
         if (isHpackError(index)) return;

         if (upd == 1 &&
             (uint32_t)index == dyntbl->hpack_max_capacity) // Omit the minimum of multiple resizes
            {
            goto upd_err;
            }

         U_INTERNAL_DUMP("hpack_capacity = %u hpack_max_capacity = %u index = %d num_entries = %u hpack_size = %u",
                          dyntbl->hpack_capacity, dyntbl->hpack_max_capacity, index, dyntbl->num_entries, dyntbl->hpack_size)
#     endif

         if ((uint32_t)index != dyntbl->hpack_capacity)
            {
            if ((uint32_t)index > dyntbl->hpack_max_capacity)
               {
               if (index > 4096) goto upd_err;

#           ifdef DEBUG
               hpack_errno = -5; // table size that exceeds the maximum
#           endif

               nerror = COMPRESSION_ERROR;

               return;
               }

            if (dyntbl->num_entries != 0 &&
                dyntbl->hpack_size > (uint32_t)index)
               {
upd_err:
#           ifdef DEBUG
               hpack_errno = -8; // A table update occurred at a wrong time or with a wrong size
#           endif

               nerror = COMPRESSION_ERROR;

               return;
               }

            setHpackDynTblCapacity(dyntbl, index);
            }

         continue;
         }

      if (dyntbl->hpack_capacity == 0)
         {
#     ifdef DEBUG
         hpack_errno = -9; // A table update was expected after a table resize but didn't occur
#     endif

         nerror = COMPRESSION_ERROR;

         return;
         }

      U_INTERNAL_DUMP("index & 0x10 = 0x%x", index & 0x10)

      /*
      if ((index & 0x10) == 0x10) // (16: 00010000) Section 6.2.3: Literal Header Field Never Indexed
         {
         ptr = hpackDecodeInt(ptr, endptr, index, (1<<4)-1); // (15: 00001111)

         goto check1;
         }
      */

      // Section 6.2.2: Literal Header Field without Indexing

      ptr = hpackDecodeInt(ptr, endptr, index, (1<<4)-1); // (15: 00001111)

#  ifdef DEBUG
      if (isHpackError(index)) return;
#  endif

check1:
      U_INTERNAL_DUMP("index = %u", index)

      if (index == 0) // not-existing name
         {
         U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)

         ptr = hpackDecodeString(ptr, endptr, name);

#     ifdef DEBUG
         if (isHpackError()) return;
#     endif

         if (name)
            {
            if (isHeaderName(name) == false)
               {
               hpack_errno = -7; // A invalid header name or value character was coded

#           ifdef DEBUG
               if (btest) return;
#           endif

               nerror = PROTOCOL_ERROR;

               return;
               }

            if (name.equal(U_CONSTANT_TO_PARAM("connection")))
               {
               nerror = PROTOCOL_ERROR;

               return;
               }

            ptr = hpackDecodeString(ptr, endptr, value);

#        ifdef DEBUG
            if (isHpackError()) return;
#        endif

            if (name.first_char() != ':')
               {
               bregular = true;

               if ( name.equal(U_CONSTANT_TO_PARAM("te")) &&
                   value.equal(U_CONSTANT_TO_PARAM("trailers")) == false)
                  {
                  nerror = PROTOCOL_ERROR;

                  return;
                  }
               }

            if (value)
               {
               table->hash = name.hashIgnoreCase();

               goto insert;
               }
            }

         nerror = COMPRESSION_ERROR;

         return;
         }

check2:
      U_INTERNAL_DUMP("index = %u", index)

      if (index >= HTTP2_HEADER_TABLE_OFFSET)
         {
         index -= HTTP2_HEADER_TABLE_OFFSET;

         U_INTERNAL_DUMP("index = %d dyntbl->num_entries = %u bvalue_is_indexed = %b binsert_dynamic_table = %b", index, dyntbl->num_entries, bvalue_is_indexed, binsert_dynamic_table)

         if (index < (int32_t)dyntbl->num_entries)
            {
            bvalue_is_indexed = false;

            entry = getHpackDynTblEntry(dyntbl, index);

            table->hash = (name = *(entry->name)).hashIgnoreCase();

            if (binsert_dynamic_table == false)
               {
               value = *(entry->value);

               goto insert_table;
               }

            if ((table->lookup(name.rep), table->node))
               {
               /**
                * A new entry can reference the name of an entry in the dynamic table that will be evicted when adding this new entry into the dynamic table.
                * Implementations are cautioned to avoid deleting the referenced name if the referenced entry is evicted from the dynamic table prior to inserting the new entry
                */

               ptr = hpackDecodeString(ptr, endptr, value);

#           ifdef DEBUG
               if (isHpackError()) return;
#           endif

               evictHpackDynTblEntry(dyntbl, entry);

               goto insertd;
               }

            binsert_dynamic_table = false;

            goto insert_table;
            }

#     ifdef DEBUG
         hpack_errno = -4; // The decoded or specified index is out of range
#     endif

         nerror = COMPRESSION_ERROR;

         return;
         }

      // existing name

      U_INTERNAL_DUMP("dispatch_table[%u] = %p &&cdefault = %p", index, dispatch_table[index], &&cdefault)

      goto *((char*)&&cdefault + dispatch_table[index]);

cdefault:
      U_INTERNAL_DUMP("index = %d bvalue_is_indexed = %b binsert_dynamic_table = %b", index, bvalue_is_indexed, binsert_dynamic_table)

      entry = hpack_static_table + --index;

      // determine the value (if necessary)

      if (bvalue_is_indexed)
         {
         bvalue_is_indexed = false;

         value = *(entry->value);
         }
      else
         {
         ptr = hpackDecodeString(ptr, endptr, value);

#     ifdef DEBUG
         if (isHpackError()) return;
#     endif
         }

      name = *(entry->name);

      table->hash = hash_static_table[index];

      U_INTERNAL_DUMP("table->hash = %u name = %V value = %V", table->hash, name.rep, value.rep)

      goto insert;

case_38: // host

      name = *UString::str_host;

      table->hash = hash_static_table[37]; // host

      goto host;

case_1: // authority (a.k.a. the Host header)

      name = *UString::str_authority;

      table->hash = hash_static_table[0]; // authority

host: U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      UHTTP::setHostname(value);

      goto insert;

case_2_3: // GET - POST

#  ifdef DEBUG
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      U_INTERNAL_DUMP("bregular = %b pseudo_header_mask = 0x%x %B", bregular, pseudo_header_mask, pseudo_header_mask)

      if (btest == false &&
          bdecodeHeadersDebug == false)
#  endif
      {
      if (bregular ||
          (pseudo_header_mask & METHOD) != 0)
         {
         nerror = PROTOCOL_ERROR;

         return;
         }
      }

      pseudo_header_mask |= METHOD;

      if (bvalue_is_indexed)
         {
         bvalue_is_indexed = false;

         U_http_method_type = (index == 2 ? HTTP_GET : (U_http_method_num = 2, HTTP_POST));

#     ifdef DEBUG
         if (btest) value = *(hpack_static_table[index-1].value);

         U_INTERNAL_DUMP("name = %V value = %V", UString::str_method->rep, hpack_static_table[index-1].value->rep)
#     endif
         }
      else
         {
         ptr = hpackDecodeString(ptr, endptr, value);

#     ifdef DEBUG
         if (isHpackError()) return;
#     endif

         switch (u_get_unalignedp32(value.data()))
            {
            case U_MULTICHAR_CONSTANT32('g','e','t','\0'):
            case U_MULTICHAR_CONSTANT32('G','E','T','\0'): U_http_method_type = HTTP_GET;                                 break;
            case U_MULTICHAR_CONSTANT32('h','e','a','d'):
            case U_MULTICHAR_CONSTANT32('H','E','A','D'):  U_http_method_type = HTTP_HEAD;        U_http_method_num =  1; break;
            case U_MULTICHAR_CONSTANT32('p','o','s','t'):
            case U_MULTICHAR_CONSTANT32('P','O','S','T'):  U_http_method_type = HTTP_POST;        U_http_method_num =  2; break;
            case U_MULTICHAR_CONSTANT32('p','u','t','\0'):
            case U_MULTICHAR_CONSTANT32('P','U','T','\0'): U_http_method_type = HTTP_PUT;         U_http_method_num =  3; break;
            case U_MULTICHAR_CONSTANT32('d','e','l','e'):
            case U_MULTICHAR_CONSTANT32('D','E','L','E'):  U_http_method_type = HTTP_DELETE;      U_http_method_num =  4; break;
            case U_MULTICHAR_CONSTANT32('o','p','t','i'):
            case U_MULTICHAR_CONSTANT32('O','P','T','I'):  U_http_method_type = HTTP_OPTIONS;     U_http_method_num =  5; break;
            // NOT IMPLEMENTED
            case U_MULTICHAR_CONSTANT32('T','R','A','C'):  U_http_method_type = HTTP_TRACE;       U_http_method_num =  6; break;
            case U_MULTICHAR_CONSTANT32('C','O','N','N'):  U_http_method_type = HTTP_CONNECT;     U_http_method_num =  7; break;
            case U_MULTICHAR_CONSTANT32('C','O','P','Y'):  U_http_method_type = HTTP_COPY;        U_http_method_num =  8; break;
            case U_MULTICHAR_CONSTANT32('M','O','V','E'):  U_http_method_type = HTTP_MOVE;        U_http_method_num =  9; break;
            case U_MULTICHAR_CONSTANT32('L','O','C','K'):  U_http_method_type = HTTP_LOCK;        U_http_method_num = 10; break;
            case U_MULTICHAR_CONSTANT32('U','N','L','O'):  U_http_method_type = HTTP_UNLOCK;      U_http_method_num = 11; break;
            case U_MULTICHAR_CONSTANT32('M','K','C','O'):  U_http_method_type = HTTP_MKCOL;       U_http_method_num = 12; break;
            case U_MULTICHAR_CONSTANT32('S','E','A','R'):  U_http_method_type = HTTP_SEARCH;      U_http_method_num = 13; break;
            case U_MULTICHAR_CONSTANT32('P','R','O','P'):  U_http_method_type = HTTP_PROPFIND;    U_http_method_num = 14; break;
            case U_MULTICHAR_CONSTANT32('P','A','T','C'):  U_http_method_type = HTTP_PATCH;       U_http_method_num = 16; break;
            case U_MULTICHAR_CONSTANT32('P','U','R','G'):  U_http_method_type = HTTP_PURGE;       U_http_method_num = 17; break;
            case U_MULTICHAR_CONSTANT32('M','E','R','G'):  U_http_method_type = HTTP_MERGE;       U_http_method_num = 18; break;
            case U_MULTICHAR_CONSTANT32('R','E','P','O'):  U_http_method_type = HTTP_REPORT;      U_http_method_num = 19; break;
            case U_MULTICHAR_CONSTANT32('C','H','E','C'):  U_http_method_type = HTTP_CHECKOUT;    U_http_method_num = 20; break;
            case U_MULTICHAR_CONSTANT32('M','K','A','C'):  U_http_method_type = HTTP_MKACTIVITY;  U_http_method_num = 21; break;
            case U_MULTICHAR_CONSTANT32('N','O','T','I'):  U_http_method_type = HTTP_NOTIFY;      U_http_method_num = 22; break;
            case U_MULTICHAR_CONSTANT32('M','S','E','A'):  U_http_method_type = HTTP_MSEARCH;     U_http_method_num = 23; break;
            case U_MULTICHAR_CONSTANT32('S','U','B','S'):  U_http_method_type = HTTP_SUBSCRIBE;   U_http_method_num = 24; break;
            case U_MULTICHAR_CONSTANT32('U','N','S','U'):  U_http_method_type = HTTP_UNSUBSCRIBE; U_http_method_num = 25; break;
            }

         U_INTERNAL_DUMP("name = %V value = %V", UString::str_method->rep, value.rep)
         }

      if (binsert_dynamic_table)
         {
         binsert_dynamic_table = false;

         addHpackDynTblEntry(dyntbl, *UString::str_method, value);
         }

#  ifdef DEBUG
      if (btest)
         {
         char buffer[4096];

         index_ptr = ptr;

         cout.write(buffer, u__snprintf(buffer, sizeof(buffer), U_CONSTANT_TO_PARAM("\n%v: %v"), UString::str_method->rep, value.rep));
         }
#  endif

      continue;

case_4_5: // :path => / /index.html

#  ifdef DEBUG
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      U_INTERNAL_DUMP("bregular = %b pseudo_header_mask = 0x%x %B", bregular, pseudo_header_mask, pseudo_header_mask)

      if (btest == false &&
          bdecodeHeadersDebug == false)
#  endif
      {
      if (bregular ||
          (pseudo_header_mask & PATH) != 0)
         {
         nerror = PROTOCOL_ERROR;

         return;
         }
      }

      pseudo_header_mask |= PATH;

      name = *UString::str_path;

      // determine the value (if necessary)

      if (bvalue_is_indexed)
         {
         bvalue_is_indexed = false;

         value = *(index == 4 ? UString::str_path_root
                              : UString::str_path_index);

         U_http_info.uri     = value.data();
         U_http_info.uri_len = value.size();

         U_INTERNAL_DUMP("URI = %.*S", U_HTTP_URI_TO_TRACE)
         }
      else
         {
         ptr = hpackDecodeString(ptr, endptr, value);

#     ifdef DEBUG
         if (isHpackError()) return;
#     endif

         setURI(value);
         }

      table->hash = hash_static_table[3]; // path

      goto insert;

case_6: // http
case_7: // https

      U_INTERNAL_ASSERT(bvalue_is_indexed)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      bvalue_is_indexed = false;

#  ifdef DEBUG
      U_INTERNAL_DUMP("name = %V value = %V", hpack_static_table[index-1].name->rep, hpack_static_table[index-1].value->rep)

      U_INTERNAL_DUMP("bregular = %b pseudo_header_mask = 0x%x %B", bregular, pseudo_header_mask, pseudo_header_mask)

      if (btest == false &&
          bdecodeHeadersDebug == false)
#  endif
      {
      if (bregular ||
          (pseudo_header_mask & SCHEME) != 0)
         {
         nerror = PROTOCOL_ERROR;

         return;
         }
      }

      pseudo_header_mask |= SCHEME;

#  ifdef DEBUG
      if (btest)
         {
         char buffer[4096];

         index_ptr = ptr;

         cout.write(buffer, u__snprintf(buffer, sizeof(buffer), U_CONSTANT_TO_PARAM("\n%v: %v"), hpack_static_table[index-1].name->rep, hpack_static_table[index-1].value->rep));
         }
#  endif

      continue;

case_8:  // :status 200
case_9:  // :status 204
case_10: // :status 206
case_11: // :status 304
case_12: // :status 400
case_13: // :status 404
case_14: // :status 500

#  ifdef DEBUG
      if (btest ||
          bdecodeHeadersDebug)
         {
         name = *UString::str_status;

         if (bvalue_is_indexed)
            {
            bvalue_is_indexed = false;

            value = *(hpack_static_table[index-1].value);
            }
         else
            {
            ptr = hpackDecodeString(ptr, endptr, value);

            if (isHpackError()) return;
            }

         table->hash = hash_static_table[7]; // status

         goto insert;
         }
#  endif

      nerror = PROTOCOL_ERROR;

      return;

case_16: // accept-encoding: gzip, deflate

      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_accept_encoding;

      // determine the value (if necessary)

      if (bvalue_is_indexed)
         {
         bvalue_is_indexed = false;

         value = *UString::str_accept_encoding_value;
         }
      else
         {
         ptr = hpackDecodeString(ptr, endptr, value);

#     ifdef DEBUG
         if (isHpackError()) return;
#     endif
         }

      setEncoding(value);

      table->hash = hash_static_table[15]; // accept_encoding

      goto insert;

case_17: // accept-language

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_accept_language;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_http_accept_language_len  = value.size();
      U_http_info.accept_language = value.data();

      U_INTERNAL_DUMP("Accept-Language: = %.*S", U_HTTP_ACCEPT_LANGUAGE_TO_TRACE)

      table->hash = hash_static_table[16]; // accept_language

      goto insert;

case_19: // accept

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_accept;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_http_accept_len  = value.size();
      U_http_info.accept = value.data();

      U_INTERNAL_DUMP("Accept: = %.*S", U_HTTP_ACCEPT_TO_TRACE)

      table->hash = hash_static_table[18]; // accept

      goto insert;

case_28: // content_length

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)

      name = *UString::str_content_length;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;

      if (bdecodeHeadersDebug == false)
#  endif
      {
      pStream->clength = value.strtoul();

      U_INTERNAL_DUMP("Content-Length: = %.*S pStream->clength = %u", U_STRING_TO_TRACE(value), pStream->clength)
      }

      table->hash = hash_static_table[27]; // content_length 

      goto insert;

case_31: // content_type

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)

      name = *UString::str_content_type;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;

      if (bdecodeHeadersDebug == false)
#  endif
      {
      U_http_content_type_len  = value.size();
      U_http_info.content_type = value.data();

      U_INTERNAL_DUMP("Content-Type(%u): = %.*S", U_http_content_type_len, U_HTTP_CTYPE_TO_TRACE)
      }

      table->hash = hash_static_table[30]; // content_type 

      goto insert;

case_32: // cookie

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_cookie;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      if (U_http_info.cookie_len) value = UString(U_CAPACITY, "%.*s; %v", U_HTTP_COOKIE_TO_TRACE, value.rep);

      U_http_info.cookie     = value.data();
      U_http_info.cookie_len = value.size();

      U_INTERNAL_DUMP("Cookie(%u): = %.*S", U_http_info.cookie_len, U_HTTP_COOKIE_TO_TRACE)

      table->hash = hash_static_table[31]; // cookie 

      goto insert;

case_35: // expect

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_INTERNAL_DUMP("name = %V value = %V", hpack_static_table[35-1].name->rep, value.rep)

      // NB: check for 'Expect: 100-continue' (as curl does)...

      bcontinue100 = value.equal(U_CONSTANT_TO_PARAM("100-continue"));

      if (bcontinue100 == false)
         {
         name = *UString::str_expect;

         table->hash = hash_static_table[34]; // expect 

         goto insert;
         }

      continue;

case_40: // if-modified-since

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_if_modified_since;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_http_info.if_modified_since = UTimeDate::getSecondFromTime(value.data(), true);

      U_INTERNAL_DUMP("If-Modified-Since = %u", U_http_info.if_modified_since)

      table->hash = hash_static_table[39]; // if_modified_since 

      goto insert;

case_50: // range 

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_range;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_http_range_len  = value.size() - U_CONSTANT_SIZE("bytes=");
      U_http_info.range = value.data() + U_CONSTANT_SIZE("bytes=");

      U_INTERNAL_DUMP("Range = %.*S", U_HTTP_RANGE_TO_TRACE)

      table->hash = hash_static_table[49]; // range 

      goto insert;

case_51: // referer 

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_referer;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_http_info.referer     = value.data();
      U_http_info.referer_len = value.size();

      U_INTERNAL_DUMP("Referer(%u): = %.*S", U_http_info.referer_len, U_HTTP_REFERER_TO_TRACE)

      table->hash = hash_static_table[50]; // referer 

      goto insert;

case_57: // transfer-encoding

      U_WARNING("Transfer-Encoding is not supported in HTTP/2");

      nerror = COMPRESSION_ERROR;

      return;

case_58: // user-agent

      U_INTERNAL_ASSERT_EQUALS(bvalue_is_indexed, false)
      U_INTERNAL_ASSERT_EQUALS(bdecodeHeadersDebug, false)

      name = *UString::str_user_agent;

      ptr = hpackDecodeString(ptr, endptr, value);

#  ifdef DEBUG
      if (isHpackError()) return;
#  endif

      U_http_info.user_agent     = value.data();
      U_http_info.user_agent_len = value.size();

      U_INTERNAL_DUMP("User-Agent: = %.*S", U_HTTP_USER_AGENT_TO_TRACE)

      table->hash = hash_static_table[57]; // user_agent

insert:
      U_INTERNAL_ASSERT(name)
      U_INTERNAL_ASSERT(value)
      U_INTERNAL_ASSERT(table->hash)

      U_INTERNAL_DUMP("dyntbl->num_entries = %u binsert_dynamic_table = %b", dyntbl->num_entries, binsert_dynamic_table)

      if (binsert_dynamic_table)
         {
insertd: binsert_dynamic_table = false;

         addHpackDynTblEntry(dyntbl, name, value);
         }

insert_table:
      table->insert(name, value); // add the decoded header to the header table

#  ifdef DEBUG
      U_INTERNAL_DUMP("name = %V value = %V", name.rep, value.rep)

      if (btest)
         {
         char buffer[4096];

         index_ptr = ptr;

         cout.write(buffer, u__snprintf(buffer, sizeof(buffer), U_CONSTANT_TO_PARAM("\n%v: %v"), name.rep, value.rep));
         }
#  endif
      }

#ifdef DEBUG
   U_INTERNAL_DUMP("bregular = %b pseudo_header_mask = 0x%x %B", bregular, pseudo_header_mask, pseudo_header_mask)

   if (btest == false &&
       bdecodeHeadersDebug == false)
#endif
   {
   if (pseudo_header_mask != (METHOD | SCHEME| PATH)) nerror = PROTOCOL_ERROR;
   }

   U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u hpack_capacity = %u hpack_max_capacity = %u",
                     dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)
}

void UHTTP2::decodeHeaders()
{
   U_TRACE(0, "UHTTP2::decodeHeaders()")

   U_DUMP("pStream->id = %u pStream->state = (%u, %s)", pStream->id, pStream->state, getStreamStatusDescription())

   UHashMap<UString>* table = &(pConnection->itable);

   U_ASSERT(table->empty())
   U_INTERNAL_ASSERT_EQUALS(U_http_version, '2')
   U_INTERNAL_ASSERT_EQUALS(U_http_info.uri_len, 0)
   U_INTERNAL_ASSERT_MAJOR(pStream->state, STREAM_STATE_OPEN)

   decodeHeaders(table, &(pConnection->idyntbl), (unsigned char*)pStream->headers.data(), (unsigned char*)pStream->headers.pend());

   if (nerror == NO_ERROR)
      {
      U_DUMP_CONTAINER(*table);

      U_INTERNAL_DUMP("Host            = %.*S", U_HTTP_HOST_TO_TRACE)
      U_INTERNAL_DUMP("Range           = %.*S", U_HTTP_RANGE_TO_TRACE)
      U_INTERNAL_DUMP("Accept          = %.*S", U_HTTP_ACCEPT_TO_TRACE)
      U_INTERNAL_DUMP("Cookie          = %.*S", U_HTTP_COOKIE_TO_TRACE)
      U_INTERNAL_DUMP("Referer         = %.*S", U_HTTP_REFERER_TO_TRACE)
      U_INTERNAL_DUMP("User-Agent      = %.*S", U_HTTP_USER_AGENT_TO_TRACE)
      U_INTERNAL_DUMP("Content-Type    = %.*S", U_HTTP_CTYPE_TO_TRACE)
      U_INTERNAL_DUMP("Accept-Language = %.*S", U_HTTP_ACCEPT_LANGUAGE_TO_TRACE)

      U_INTERNAL_DUMP("U_http_is_accept_gzip = %b", U_http_is_accept_gzip)

      if (U_http_is_accept_gzip == false)
         {
         table->hash = hash_static_table[15]; // accept-encoding

         UString value = table->at(UString::str_accept_encoding->rep);

         U_INTERNAL_DUMP("value = %V", value.rep)

         if (value) setEncoding(value);
         }

      U_INTERNAL_DUMP("URI(%u) = %.*S", U_http_info.uri_len, U_HTTP_URI_TO_TRACE)

      if (U_http_info.uri_len == 0)
         {
         table->hash = hash_static_table[3]; // path

         UString value = table->at(UString::str_path->rep);

         U_INTERNAL_DUMP("value = %V", value.rep)

         if (value) setURI(value);
         }

      U_INTERNAL_DUMP("U_http_method_type = %B U_http_method_num = %d", U_http_method_type, U_http_method_num)
      }
}

void UHTTP2::manageHeaders()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::manageHeaders()")

   U_ASSERT(pStream->body.empty())
   U_ASSERT(pStream->headers.empty())
   U_INTERNAL_ASSERT_EQUALS(pStream->id, 0)
   U_INTERNAL_ASSERT_EQUALS(pStream->clength, 0)
   U_INTERNAL_ASSERT_EQUALS(pStream->state, STREAM_STATE_IDLE)

   uint32_t padlen, sz;
   unsigned char* ptr = frame.payload;
   unsigned char* endptr = ptr + frame.length;

   if ((frame.flags & FLAG_PADDED) == 0) padlen = 0;
   else
      {
      padlen = *ptr++;

      U_INTERNAL_DUMP("padlen = %u", padlen)

      if (frame.length < padlen)
         {
         nerror = PROTOCOL_ERROR;

         return;
         }

      endptr -= padlen;
      }

   if ((frame.flags & FLAG_PRIORITY) == 0)
      {
      priority_weight     = 0;
      priority_exclusive  = false;
      priority_dependency = 0;
      }
   else
      {
      if ((frame.length - padlen) < 5)
         {
         nerror = PROTOCOL_ERROR;

         return;
         }

      readPriority(ptr);

      if (nerror != NO_ERROR) return;

      // TODO

      ptr += 5;
      }

   U_INTERNAL_DUMP("nerror = %u", nerror)

   U_INTERNAL_ASSERT_EQUALS(nerror, NO_ERROR)

   if (pStream->headers.empty() == false)
      {
      U_INTERNAL_DUMP("pStream->headers = %V", pStream->headers.rep)

      if ((frame.flags & FLAG_END_STREAM) != 0)
         {
         // check for trailer part

         UHashMap<UString>* table = &(pConnection->itable);

         table->hash = u_hash_ignore_case((unsigned char*)U_CONSTANT_TO_PARAM("trailer"));

         UString trailer = table->at(U_CONSTANT_TO_PARAM("trailer"));

         U_INTERNAL_DUMP("trailer = %V", trailer.rep)

         if (trailer)
            {
            UHashMap<UString> tmp;

            decodeHeaders(&tmp, &(pConnection->idyntbl), ptr, endptr);

            UString trailer_value = table->at(trailer.rep);

            U_INTERNAL_DUMP("trailer_value = %V", trailer_value.rep)

            if (trailer_value) goto next;
            }
         }

      nerror = PROTOCOL_ERROR;

      return;
      }

next:
   sz = (endptr - ptr);

   pStream->state = ((frame.flags & FLAG_END_STREAM) != 0 ? STREAM_STATE_HALF_CLOSED : STREAM_STATE_OPEN);

   if ((frame.flags & FLAG_END_HEADERS) != 0)
      {
      (void) pStream->headers.replace((const char*)ptr, sz);

      return;
      }

   // we must wait for CONTINUATION frames for the same stream...

   U_INTERNAL_ASSERT_EQUALS(frame.flags & FLAG_END_HEADERS, 0)

   pStream->headers = UString(ptr, sz, sz * 2);

   wait_for_continuation = frame.stream_id;
}

void UHTTP2::setStream()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::setStream()")

   U_DUMP("frame.stream_id = %u pConnection->max_processed_stream_id = %u pStream->id = %u pStream->state = (%u, %s)",
           frame.stream_id,     pConnection->max_processed_stream_id,     pStream->id,     pStream->state, getStreamStatusDescription())

   if (pStream->id == frame.stream_id)
      {
chk:  if (pStream->state < STREAM_STATE_HALF_CLOSED)
         {
         if ((frame.flags & FLAG_END_STREAM) != 0) pStream->state = STREAM_STATE_HALF_CLOSED;
         else
            {
            if (frame.type == HEADERS) nerror = PROTOCOL_ERROR;
            }
         }
      else
         {
         if (frame.type != WINDOW_UPDATE) nerror = STREAM_CLOSED;
         }

      return;
      }

   if (pStream->id == 0)
      {
      if (frame.stream_id <= pConnection->max_processed_stream_id)
         {
         nerror = STREAM_CLOSED;

         return;
         }

manage_headers:
      if (frame.type != HEADERS)
         {
         nerror = PROTOCOL_ERROR;

         return;
         }

      manageHeaders();

      U_INTERNAL_DUMP("nerror = %u", nerror)

      if (nerror == NO_ERROR)
         {
         pStream->id = frame.stream_id;

         if (pConnection->max_processed_stream_id < frame.stream_id) pConnection->max_processed_stream_id = frame.stream_id;
         }

      return;
      }

   if (frame.stream_id > pConnection->max_processed_stream_id)
      {
      if (++pStream >= (pConnection->streams+HTTP2_MAX_CONCURRENT_STREAMS))
         {
         nerror = REFUSED_STREAM;

         return;
         }

      ++pStreamEnd;

      goto manage_headers;
      }

   for (pStream = pConnection->streams; pStream <= pStreamEnd; ++pStream)
      {
      U_DUMP("pStream->id = %u pStream->state = (%u, %s)", pStream->id, pStream->state, getStreamStatusDescription())

      if (pStream->id == frame.stream_id) goto chk;
      }

   nerror = PROTOCOL_ERROR;
}

void UHTTP2::readFrame()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::readFrame()")

   U_INTERNAL_ASSERT_EQUALS(nerror, NO_ERROR)

   int32_t len;
   uint32_t error;
   const char* descr;
   const unsigned char* ptr;

loop:
   U_INTERNAL_DUMP("UClientImage_Base::rbuffer->size() = %u UClientImage_Base::rstart = %u", UClientImage_Base::rbuffer->size(), UClientImage_Base::rstart)

   if ((UClientImage_Base::rbuffer->size() - UClientImage_Base::rstart) < HTTP2_FRAME_HEADER_SIZE)
      {
      if (UClientImage_Base::rbuffer->size() == UClientImage_Base::rstart) resetDataRead();

      if (USocketExt::read(UServer_Base::csocket, *UClientImage_Base::rbuffer, U_SINGLE_READ, U_SSL_TIMEOUT_MS, UHTTP::request_read_timeout) == false)
         {
         nerror = ERROR_INCOMPLETE;

         goto end;
         }
      }

   ptr = (const unsigned char*) UClientImage_Base::rbuffer->c_pointer(UClientImage_Base::rstart);

   frame.length    = ptr[0] << 16 |
                     ptr[1] <<  8 |
                     ptr[2];
   frame.type      = ptr[3];
   frame.flags     = ptr[4];
   frame.stream_id = ntohl(*(uint32_t*)(ptr+5)) & 0x7fffffff;

   U_DUMP("frame { length = %d stream_id = %d type = (%u, %s) flags = %d %B } = %#.*S", frame.length,
           frame.stream_id, frame.type, getFrameTypeDescription(frame.type), frame.flags, frame.flags, frame.length, ptr + HTTP2_FRAME_HEADER_SIZE)

   U_INTERNAL_ASSERT_EQUALS(frame.length, (ntohl(*(uint32_t*)ptr) & 0x00ffffff) >> 8)

   if (frame.length > pConnection->peer_settings.max_frame_size)
      {
      nerror = FRAME_SIZE_ERROR;

      goto end;
      }

   len = UClientImage_Base::rbuffer->size() - (UClientImage_Base::rstart += HTTP2_FRAME_HEADER_SIZE) - frame.length;

   U_INTERNAL_DUMP("UClientImage_Base::rbuffer->size() = %u UClientImage_Base::rstart = %u frame.length = %u len = %d",
                    UClientImage_Base::rbuffer->size(),     UClientImage_Base::rstart,     frame.length,     len)

   if (len < 0)
      {
      len = -len;

      if (UClientImage_Base::rstart &&
          UClientImage_Base::rstart < UClientImage_Base::rbuffer->size())
         {
         UClientImage_Base::rbuffer->moveToBeginDataInBuffer(UClientImage_Base::rstart); 
                                                             UClientImage_Base::rstart = 0;
         }

      if (USocketExt::read(UServer_Base::csocket, *UClientImage_Base::rbuffer, len, U_SSL_TIMEOUT_MS, UHTTP::request_read_timeout) == false)
         {
         nerror = ERROR_INCOMPLETE;

         goto end;
         }
      }

   frame.payload = (unsigned char*) UClientImage_Base::rbuffer->c_pointer(UClientImage_Base::rstart);
                                                                          UClientImage_Base::rstart += (uint32_t)frame.length;

   U_INTERNAL_DUMP("wait_for_continuation = %u", wait_for_continuation)

   if (frame.type > CONTINUATION)
      {
      if (wait_for_continuation == 0) goto ret; // The endpoint MUST discard frames that have unknown or unsupported types

      nerror = PROTOCOL_ERROR;

      goto end;
      }

   if (wait_for_continuation)
      {
      if (frame.type == CONTINUATION)
         {
         if (wait_for_continuation == frame.stream_id)
            {
            (void) pStream->headers.append((const char*)frame.payload, frame.length);

            if ((frame.flags & FLAG_END_HEADERS) == 0) goto loop;

            wait_for_continuation = 0;

            return;
            }
         }

      nerror = PROTOCOL_ERROR;

      goto end;
      }

   if (frame.type == SETTINGS)
      {
      if (frame.stream_id)
         {
         nerror = PROTOCOL_ERROR;

         goto end;
         }

      if ((frame.flags & FLAG_ACK) != 0)
         {
         if (frame.length)
            {
            nerror = FRAME_SIZE_ERROR;

            goto end;
            }

         bsetting_ack = true;

         goto ret;
         }

      if (frame.length)
         {
         updateSetting(frame.payload, frame.length);

         if (nerror != NO_ERROR) goto end;
         }

      U_INTERNAL_DUMP("bsetting_send = %b", bsetting_send)

      if (bsetting_send) (void) USocketExt::write(UServer_Base::csocket, U_CONSTANT_TO_PARAM(HTTP2_SETTINGS_ACK), 0);
      else
         {
         bsetting_send = true;

         (void) USocketExt::write(UServer_Base::csocket, U_CONSTANT_TO_PARAM(HTTP2_SETTINGS_BIN HTTP2_SETTINGS_ACK), 0);
         }

      U_INTERNAL_DUMP("pConnection->inp_window = %d pConnection->out_window = %d", pConnection->inp_window, pConnection->out_window)

      goto ret;
      }

   if (frame.stream_id == 0) // stream ID zero is for connection-oriented stuff
      {
      if (frame.type == WINDOW_UPDATE)
         {
window_update:
         if (frame.length != 4)
            {
            nerror = FRAME_SIZE_ERROR;

            goto end;
            }

         uint32_t window_size_increment = ntohl(*(uint32_t*)frame.payload) & 0x7fffffff;

         U_INTERNAL_DUMP("pConnection->out_window = %d window_size_increment = %u", pConnection->out_window, window_size_increment)

         if (window_size_increment == 0)
            {
            nerror = PROTOCOL_ERROR;

            goto end;
            }

         if ((pConnection->out_window + window_size_increment) > HTTP2_MAX_WINDOW_SIZE)
            {
            nerror = FLOW_CONTROL_ERROR;

            goto end;
            }

         if (frame.stream_id) pConnection->out_window += window_size_increment;

         goto ret;
         }

      if (frame.type == GOAWAY)
         {
         error = ntohl(*(uint32_t*)(frame.payload+4));
         descr = getFrameErrorCodeDescription(error);

         U_DEBUG("Received GOAWAY frame with error (%u, %s)", error, descr)

         U_SRV_LOG("received GOAWAY frame with error (%u, %s)", error, descr);

         U_INTERNAL_DUMP("GOAWAY: Last-Stream-ID = %u", ntohl(*(uint32_t*)frame.payload) & 0x7fffffff)

         goto ret;
         }

      if (frame.type == PING)
         {
         if (frame.length != 8)
            {
            nerror = FRAME_SIZE_ERROR;

            goto end;
            }

         if (frame.flags == 0)
            {
            U_DEBUG("Received PING frame with data (%#.8S)", frame.payload)

            U_SRV_LOG("received PING frame with data (%#.8S)", frame.payload);

            sendPing();
            }

         goto ret;
         }

      nerror = PROTOCOL_ERROR;

      goto end;
      }

   if ((frame.stream_id & 1) == 0)
      {
      nerror = PROTOCOL_ERROR;

      goto end;
      }

   if (frame.type == PRIORITY)
      {
      if (frame.length != 5) nerror = FRAME_SIZE_ERROR;
      else
         {
         readPriority(frame.payload);

         if (nerror == NO_ERROR)
            {
            // TODO

            goto ret;
            }
         }

      goto end;
      }

   setStream();

   U_INTERNAL_DUMP("nerror = %u", nerror)

   if (nerror != NO_ERROR) goto end;

   if (wait_for_continuation)
      {
      U_INTERNAL_DUMP("wait_for_continuation = %u", wait_for_continuation)

      goto loop;
      }

   if (frame.type == HEADERS) goto ret;

   if (frame.type == DATA)
      {
      if (frame.length)
         {
         if ((frame.flags & FLAG_PADDED) == 0) (void) pStream->body.append((const char*)frame.payload, frame.length);
         else
            {
            uint32_t padlen = *(frame.payload);

            U_INTERNAL_DUMP("frame.length = %u padlen = %u", frame.length, padlen)

            if (frame.length < padlen)
               {
               nerror = PROTOCOL_ERROR;

               goto end;
               }

            (void) pStream->body.append((const char*)frame.payload+1, frame.length-padlen-1);
            }

         if ((frame.flags & FLAG_END_STREAM) != 0) goto ret;

         // we must wait for other DATA frames for the same stream...

         U_INTERNAL_DUMP("frame.length = %u pConnection->inp_window = %d", frame.length, pConnection->inp_window)

         if ((pConnection->inp_window -= frame.length) <= 0) // Send Window Update if current window size is not sufficient
            {
            pConnection->inp_window = (HTTP2_MAX_WINDOW_SIZE + pConnection->inp_window);

            sendWindowUpdate();
            }

         goto loop;
         }

      goto ret;
      }

   if (frame.type == WINDOW_UPDATE)
      {
      if (pStream->state == STREAM_STATE_IDLE)
         {
         nerror = PROTOCOL_ERROR;

         goto end;
         }

      goto window_update;
      }

   if (frame.type == RST_STREAM)
      {
      if (frame.length != 4)
         {
         nerror = FRAME_SIZE_ERROR;

         goto end;
         }

      error = ntohl(*(uint32_t*)frame.payload);
      descr = getFrameErrorCodeDescription(error);

      U_DEBUG("Received RST_STREAM frame for stream %u with error (%u, %s)", frame.stream_id, error, descr)

      U_SRV_LOG("received RST_STREAM frame for stream %u with error (%u, %s)", frame.stream_id, error, descr);

      pStream->state = STREAM_STATE_CLOSED;

      goto ret;
      }

   U_DUMP("frame.type = (%u, %s)", frame.type, getFrameTypeDescription(frame.type))

   nerror = PROTOCOL_ERROR;

end:
   U_INTERNAL_DUMP("nerror = %u", nerror)

   resetDataRead();

   return;

ret:
   U_INTERNAL_DUMP("pConnection->out_window = %d UClientImage_Base::rbuffer->size() = %u UClientImage_Base::rstart = %u",
                    pConnection->out_window,     UClientImage_Base::rbuffer->size(),     UClientImage_Base::rstart)

   if (pConnection->out_window == 0 ||
       UClientImage_Base::rbuffer->size() > UClientImage_Base::rstart)
      {
      goto loop;
      }
}

unsigned char* UHTTP2::hpackEncodeHeader(unsigned char* dst, const UString& name, const UString& value)
{
   U_TRACE(0, "UHTTP2::hpackEncodeHeader(%p,%V,%V)", dst, name.rep, value.rep)

   int32_t index = 0;
   const char* keyp = name.data();

   switch (u_get_unalignedp32(keyp))
      {
      case U_MULTICHAR_CONSTANT32('C','o','n','t'):
         {
         keyp += 7;

         switch (u_get_unalignedp32(keyp))
            {
            case U_MULTICHAR_CONSTANT32('-','D','i','s'): index = 25; break; // content-disposition
            case U_MULTICHAR_CONSTANT32('-','E','n','c'): index = 26; break; // content-encoding
            case U_MULTICHAR_CONSTANT32('-','L','a','n'): index = 27; break; // content-language
            case U_MULTICHAR_CONSTANT32('-','L','e','n'): index = 28; break; // content-length
            case U_MULTICHAR_CONSTANT32('-','L','o','c'): index = 29; break; // content-location
            case U_MULTICHAR_CONSTANT32('-','R','a','n'): index = 30; break; // content-range
            case U_MULTICHAR_CONSTANT32('-','T','y','p'): index = 31; break; // content-type 

#        ifdef DEBUG
            default:
               {
               U_ASSERT(name == U_STRING_FROM_CONSTANT("Content-Style-Type") ||
                        name == U_STRING_FROM_CONSTANT("Content-Script-Type"))

               U_ERROR("Content-Style-Type and Content-Script-Type are not supported");
               }
#        endif
            }
         }
      break;

      case U_MULTICHAR_CONSTANT32('A','c','c','e'):
         {
         keyp += 6;

         if (u_get_unalignedp32(keyp) == U_MULTICHAR_CONSTANT32('-','R','a','n')) index = 18; // accept-ranges
         else
            {
            U_INTERNAL_ASSERT_EQUALS(name, U_STRING_FROM_CONSTANT("Access-Control-Allow-Origin"))

            index = 20; // access-control-allow-origin
            }
         }
      break;

      case U_MULTICHAR_CONSTANT32('C','a','c','h'): index = 24; break; // cache-control
      case U_MULTICHAR_CONSTANT32('E','t','a','g'): index = 34; break; // etag
      case U_MULTICHAR_CONSTANT32('E','x','p','i'): index = 36; break; // expires
      case U_MULTICHAR_CONSTANT32('L','a','s','t'): index = 44; break; // last-modified
      case U_MULTICHAR_CONSTANT32('L','i','n','k'): index = 45; break; // link
      case U_MULTICHAR_CONSTANT32('L','o','c','a'): index = 46; break; // location
      case U_MULTICHAR_CONSTANT32('P','r','o','x'): index = 48; break; // proxy-authenticate
      case U_MULTICHAR_CONSTANT32('R','e','f','r'): index = 52; break; // refresh
      case U_MULTICHAR_CONSTANT32('R','e','t','r'): index = 53; break; // retry-after
      case U_MULTICHAR_CONSTANT32('S','t','r','i'): index = 56; break; // strict-transport-security
      case U_MULTICHAR_CONSTANT32('V','a','r','y'): index = 59; break; // vary
      case U_MULTICHAR_CONSTANT32('W','W','W','-'): index = 61; break; // www-authenticate 

      default:
         {
              if (name.equalnocase(U_CONSTANT_TO_PARAM("Via"))) index = 60; // via
         else if (name.equalnocase(U_CONSTANT_TO_PARAM("Age"))) index = 21; // age
         }
      break;
      }

   if (index) dst = hpackEncodeInt(dst, index, (1<<4)-1, 0x00); // literal
   else
      {
      char* ptr = name.data();

      if (u__isupper(*ptr))
         {
         for (char* end = ptr + name.size(); ptr < end; ++ptr)
            {
            char c = *ptr;

            if (u__isupper(c)) *ptr = u__tolower(c);
            }
         }

#  ifdef DEBUG
      if (isHeaderName(name) == false)
         {
         hpack_errno = -7; // A invalid header name or value character was coded

         U_RETURN_POINTER(dst, unsigned char);
         }
#  endif

      *dst++ = 0;
       dst   = hpackEncodeString(dst, name, true); // not-existing name
      }

   dst = hpackEncodeString(dst, value, true);

   U_RETURN_POINTER(dst, unsigned char);
}

void UHTTP2::handlerResponse()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::handlerResponse()")

   U_INTERNAL_ASSERT(bsetting_ack)
   U_INTERNAL_ASSERT(bsetting_send)
   U_INTERNAL_ASSERT_MAJOR(pStream->state, STREAM_STATE_OPEN)

   uint32_t sz1 = UHTTP::set_cookie->size(),
            sz2 = UHTTP::ext->size();

   U_INTERNAL_DUMP("ext(%u) = %#V", sz2, UHTTP::ext->rep)

   UClientImage_Base::wbuffer->setBuffer(800U + sz1 + sz2);

   unsigned char* dst = (unsigned char*)UClientImage_Base::wbuffer->data();

   if (U_http_info.nResponseCode == HTTP_NOT_IMPLEMENTED ||
       U_http_info.nResponseCode == HTTP_OPTIONS_RESPONSE)
      {
      UClientImage_Base::body->clear(); // clean body to avoid writev() in response...

      UClientImage_Base::setCloseConnection();

      /**
       * dst = hpackEncodeInt(dst, 8, (1<<7)-1, 0x80);
       */

      *dst++ = 0x80 | 8; // HTTP_OK

      /**
       * dst = hpackEncodeInt(dst, 22, (1<<4)-1, 0x00);
       */

      u_put_unalignedp16(dst, U_MULTICHAR_CONSTANT16(0x0f,0x07));
                         dst += 2;

      dst = hpackEncodeString(dst,
               U_CONSTANT_TO_PARAM("GET, HEAD, POST, PUT, DELETE, OPTIONS, "     // request methods
                                   "TRACE, CONNECT, "                            // pathological
                                   "COPY, MOVE, LOCK, UNLOCK, MKCOL, PROPFIND, " // webdav
                                   "PATCH, PURGE, "                              // rfc-5789
                                   "MERGE, REPORT, CHECKOUT, MKACTIVITY, "       // subversion
                                   "NOTIFY, MSEARCH, SUBSCRIBE, UNSUBSCRIBE"),   // upnp
                                   true);
      }
   else
      {
      if (sz2 == 0 &&
          U_http_info.nResponseCode == HTTP_OK)
         {
         *dst++ = 0x80 | 9; // HTTP_NO_CONTENT
         }
      else
         {
         switch (U_http_info.nResponseCode)
            {
            case HTTP_OK:             *dst++ = 0x80 |  8; break;
            case HTTP_NO_CONTENT:     *dst++ = 0x80 |  9; break;
            case HTTP_PARTIAL:        *dst++ = 0x80 | 10; break;
            case HTTP_NOT_MODIFIED:   *dst++ = 0x80 | 11; break;
            case HTTP_BAD_REQUEST:    *dst++ = 0x80 | 12; break;
            case HTTP_NOT_FOUND:      *dst++ = 0x80 | 13; break;
            case HTTP_INTERNAL_ERROR: *dst++ = 0x80 | 14; break;

            default:
               {
               /**
                * dst = hpackEncodeInt(dst, 8, (1<<4)-1, 0x00);
                * dst = hpackEncodeString(dst, U_CONSTANT_TO_PARAM("403"), false);
                */

               u_put_unalignedp32(dst, U_MULTICHAR_CONSTANT32(0x08,0x03,0x30+(U_http_info.nResponseCode / 100),'\0'));
                      U_NUM2STR16(dst+3,                                      U_http_info.nResponseCode % 100);

               dst += 5;
               }
            }
         }
      }

   /**
    * server: ULib
    * date: Wed, 20 Jun 2012 11:43:17 GMT
    */

   HpackDynamicTable* dyntbl = &(pConnection->odyntbl);

   U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u hpack_capacity = %u hpack_max_capacity = %u",
                     dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)

   if (dyntbl->num_entries == 0)
      {
#  if defined(U_LINUX) && defined(ENABLE_THREAD) && defined(U_LOG_DISABLE) && !defined(USE_LIBZ)
      U_INTERNAL_ASSERT_POINTER(u_pthread_time)
      U_INTERNAL_ASSERT_EQUALS(UClientImage_Base::iov_vec[1].iov_base, ULog::ptr_shared_date->date3)
#  else
      U_INTERNAL_ASSERT_EQUALS(UClientImage_Base::iov_vec[1].iov_base, ULog::date.date3)

      ULog::updateDate3(0);
#  endif

      UString date((void*)(((char*)UClientImage_Base::iov_vec[1].iov_base)+6), 29);

      /**
       * dst = hpackEncodeInt(dst, 54, (1<<6)-1, 0x40);
       * dst = hpackEncodeString(dst, U_CONSTANT_TO_PARAM("ULib"), false);
       * dst = hpackEncodeInt(dst, 33, (1<<6)-1, 0x40);
       */

      u_put_unalignedp64(dst, U_MULTICHAR_CONSTANT64(0x76,0x04,0x55,0x4C,0x69,0x62,0x61,'\0'));

      dst = hpackEncodeString(dst+7, date.data(), 29, true); // Date: Wed, 20 Jun 2012 11:43:17 GMT\r\nServer: ULib\r\nConnection: close\r\n

      addHpackDynTblEntry(dyntbl, *UString::str_server, *UString::str_ULib);
      addHpackDynTblEntry(dyntbl, *UString::str_date, date);
      }
   else
      {
      HpackHeaderTableEntry* entry = getHpackDynTblEntry(dyntbl, 0); // last entry

      char* ptr_date = entry->value->data();

#  if defined(U_LINUX) && defined(ENABLE_THREAD) && defined(U_LOG_DISABLE) && !defined(USE_LIBZ)
      U_INTERNAL_ASSERT_POINTER(u_pthread_time)
      U_INTERNAL_ASSERT_EQUALS(UClientImage_Base::iov_vec[1].iov_base, ULog::ptr_shared_date->date3)
#  else
      U_INTERNAL_ASSERT_EQUALS(UClientImage_Base::iov_vec[1].iov_base, ULog::date.date3)

      ULog::updateDate3(ptr_date);
#  endif

      U_ASSERT(entry->name->equal(*UString::str_date))

      /**
       * dst = hpackEncodeInt(dst, 1+HTTP2_HEADER_TABLE_OFFSET, (1<<7)-1, 0x80);
       */

      *dst++ = 0xbf;

      if (ULog::tv_sec_old_3 == u_now->tv_sec)
         {
         /**
          * dst = hpackEncodeInt(dst, 0+HTTP2_HEADER_TABLE_OFFSET, (1<<7)-1, 0x80);
          */

         *dst++ = 0xbe;
         }
      else
         {
         /**
          * dst = hpackEncodeInt(dst, 0+HTTP2_HEADER_TABLE_OFFSET, (1<<6)-1, 0x40);
          */

         *dst++ = 0x7e;
          dst   = hpackEncodeString(dst, ptr_date, 29, true); // Date: Wed, 20 Jun 2012 11:43:17 GMT\r\nServer: ULib\r\nConnection: close\r\n
         }
      }

   if (sz1)
      {
      /**
       * dst = hpackEncodeInt(dst, 55, (1<<4)-1, 0x00);
       */

      u_put_unalignedp16(dst, U_MULTICHAR_CONSTANT16(0x0f,0x28));
                         dst += 2;

      dst = hpackEncodeString(dst, UHTTP::set_cookie->data(), sz1, true);

      UHTTP::set_cookie->setEmpty();
      }

   if (sz2)
      {
      UString row, key;
      UVector<UString> vext(20);

      for (uint32_t i = 0, n = vext.split(*UHTTP::ext, U_CRLF); i < n; ++i)
         {
         row = vext[i];

         uint32_t pos = row.find_first_of(':');

         dst = hpackEncodeHeader(dst, row.substr(0U, pos), row.substr(pos+2)); 
         }
      }
   else // content-length: 0
      {
      /**
       * dst = hpackEncodeInt(dst, 28, (1<<4)-1, 0x00);
       * dst = hpackEncodeString(dst, U_CONSTANT_TO_PARAM("0"), false);
       */

      u_put_unalignedp32(dst, U_MULTICHAR_CONSTANT32(0x0f,0x0d,0x01,0x30));
                         dst += 4;
      }

   UClientImage_Base::wbuffer->size_adjust((const char*)dst);

   U_INTERNAL_ASSERT_MINOR(UClientImage_Base::wbuffer->size(), pConnection->peer_settings.max_frame_size)
}

void UHTTP2::writeData(struct iovec* iov, bool bdata, bool flag)
{
   U_TRACE(0, "UHTTP2::writeData(%p,%b,%b,%b)", iov, bdata, flag)

   char* ptr               = (char*)iov[3].iov_base;
   char* ptr2              = (char*)iov[2].iov_base;
   uint32_t sz1            =        iov[1].iov_len,
            sz2            =        iov[3].iov_len,
            max_frame_size = pConnection->peer_settings.max_frame_size;

   if (sz2 <= max_frame_size)
      {
      *(uint32_t*) ptr2    = htonl(sz2 << 8);
                   ptr2[3] = DATA;
                   ptr2[4] = flag;

      U_DUMP("frame data response { length = %d stream_id = %d type = (%d, %s) flags = %d } = %#.*S",
               (ntohl(*(uint32_t*) ptr2) & 0x00ffffff) >> 8,
                ntohl(*(uint32_t*)(ptr2+5)) & 0x7fffffff, ptr2[3], getFrameTypeDescription(ptr2[3]), ptr2[4], (ntohl(*(uint32_t*)ptr2) & 0x00ffffff) >> 8, ptr)

      if (bdata) writev(iov,   4, HTTP2_FRAME_HEADER_SIZE+sz1+HTTP2_FRAME_HEADER_SIZE+sz2);
      else       writev(iov+2, 2,                             HTTP2_FRAME_HEADER_SIZE+sz2);

      return;
      }

   *(uint32_t*) ptr2    = htonl(max_frame_size << 8);
                ptr2[3] = DATA;
                ptr2[4] = 0;

   iov[3].iov_len = max_frame_size;

   if (bnghttp2)
      {
      if (bdata) writev(iov,   4, HTTP2_FRAME_HEADER_SIZE+sz1+HTTP2_FRAME_HEADER_SIZE+max_frame_size);
      else       writev(iov+2, 2,                             HTTP2_FRAME_HEADER_SIZE+max_frame_size);

loop1:
      iov[3].iov_base = (ptr += max_frame_size);
                         sz2 -= max_frame_size;

      U_INTERNAL_DUMP("pConnection->peer_settings.max_frame_size = %u sz2 = %u", max_frame_size, sz2)

      if (sz2 > max_frame_size)
         {
         writev(iov+2, 2, HTTP2_FRAME_HEADER_SIZE+max_frame_size);

         goto loop1;
         }

      *(uint32_t*) ptr2    = htonl(sz2 << 8);
                   ptr2[3] = DATA;
                   ptr2[4] = flag;

      iov[3].iov_len = sz2;

      writev(iov+2, 2, HTTP2_FRAME_HEADER_SIZE+sz2);

      return;
      }

   uint32_t iovcnt, count;
   struct iovec iov_vec[1024];

   if (bdata)
      {
      U_MEMCPY(iov_vec, iov, sizeof(struct iovec) * (iovcnt = 4));

      count = HTTP2_FRAME_HEADER_SIZE+sz1+HTTP2_FRAME_HEADER_SIZE+max_frame_size;
      }
   else
      {
      U_MEMCPY(iov_vec, iov+2, sizeof(struct iovec) * (iovcnt = 2));

      count = HTTP2_FRAME_HEADER_SIZE+max_frame_size;
      }

loop2:
   ptr += max_frame_size;

   if ((sz2 -= max_frame_size) > max_frame_size)
      {
      U_MEMCPY(iov_vec+iovcnt, iov+2, sizeof(struct iovec) * 2);

      ++iovcnt;

                               iov_vec[iovcnt].iov_base = ptr;
      U_INTERNAL_ASSERT_EQUALS(iov_vec[iovcnt].iov_len, max_frame_size)

      ++iovcnt;

      U_INTERNAL_ASSERT_MINOR(iovcnt, 1024)

      count += HTTP2_FRAME_HEADER_SIZE+max_frame_size;

      goto loop2;
      }

   iov_vec[iovcnt] = iov[2];

   ++iovcnt;

   iov_vec[iovcnt].iov_len  = sz2;
   iov_vec[iovcnt].iov_base = ptr;

   ++iovcnt;

   U_INTERNAL_ASSERT_MINOR(iovcnt, 1024)

   count += HTTP2_FRAME_HEADER_SIZE+sz2;

   writev(iov_vec, iovcnt, count);
}

void UHTTP2::writeResponse()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::writeResponse()")

   char buffer1[HTTP2_FRAME_HEADER_SIZE];
   char buffer2[HTTP2_FRAME_HEADER_SIZE];

   char* ptr  = UClientImage_Base::body->data();
   char* ptr0 = UClientImage_Base::wbuffer->data();
   char* ptr1 = buffer1;

   uint32_t sz0     = UClientImage_Base::wbuffer->size(),
            body_sz = UClientImage_Base::body->size();

   struct iovec iov_vec[4] = { { (caddr_t)buffer1, HTTP2_FRAME_HEADER_SIZE },
                               { (caddr_t)ptr0, sz0 },
                               { (caddr_t)buffer2, HTTP2_FRAME_HEADER_SIZE },
                               { (caddr_t)ptr, body_sz } };

   U_INTERNAL_DUMP("UClientImage_Base::wbuffer(%u) = %V", sz0, UClientImage_Base::wbuffer->rep)

   U_INTERNAL_ASSERT_MAJOR((int)pStream->id, 0)
   U_INTERNAL_ASSERT(*UClientImage_Base::wbuffer)

   *(uint32_t*) ptr1    = htonl(sz0 << 8);
                ptr1[3] = HEADERS;
                ptr1[4] = FLAG_END_HEADERS | (body_sz == 0); // FLAG_END_STREAM (1)
   *(uint32_t*)(ptr1+5) = htonl(pStream->id);

   U_DUMP("frame header response { length = %d stream_id = %d type = (%d, %s) flags = %d } = %#.*S",
            (ntohl(*(uint32_t*) ptr1) & 0x00ffffff) >> 8,
             ntohl(*(uint32_t*)(ptr1+5)) & 0x7fffffff, ptr1[3], getFrameTypeDescription(ptr1[3]), ptr1[4], (ntohl(*(uint32_t*)ptr1) & 0x00ffffff) >> 8, ptr0)

   U_SRV_LOG_WITH_ADDR("send response (%s,id:%u,bytes:%u) %#.*S to", bnghttp2 ? "nghttp2" : "HTTP2", pStream->id, sz0+body_sz, sz0, ptr0);

   if (body_sz == 0)
      {
      writev(iov_vec, 2, HTTP2_FRAME_HEADER_SIZE+sz0);

#  ifdef DEBUG
   // decodeHeadersResponse(ptr0, sz0);
#  endif

      return;
      }

   U_INTERNAL_ASSERT_MAJOR(body_sz, 0)

   char* ptr2 = buffer2;

   *(uint32_t*)(ptr2+5) = htonl(pStream->id);

   if ((uint32_t)pConnection->out_window >= body_sz)
      {
      pConnection->out_window -= body_sz;

      writeData(iov_vec, true, FLAG_END_STREAM);

      return;
      }

   U_DEBUG("Current window size (%d) is not sufficient for data size: %u", pConnection->out_window, body_sz)

   U_SRV_LOG("WARNING: Current window size (%d) is not sufficient for data size: %u", pConnection->out_window, body_sz);

   if (pConnection->out_window == 0) writev(iov_vec, 2, HTTP2_FRAME_HEADER_SIZE+sz0);
   else
      {
      ptr               += pConnection->out_window;
      body_sz           -= pConnection->out_window;
      iov_vec[3].iov_len = pConnection->out_window;
                           pConnection->out_window = 0;

      writeData(iov_vec, true, false);
      }

   // we must wait for a Window Update frame if the current window size is not sufficient...

#ifdef DEBUG
   Stream* pStreamEndOld = pStreamEnd;
#endif

loop:
   readFrame();

   U_INTERNAL_DUMP("nerror = %u", nerror)

   if (nerror == NO_ERROR)
      {
      U_INTERNAL_ASSERT_MAJOR(pConnection->out_window, 0)
      U_INTERNAL_ASSERT_EQUALS(pStreamEnd, pStreamEndOld)

      iov_vec[3].iov_base = ptr;

      if (body_sz > (uint32_t)pConnection->out_window)
         {
         ptr               += pConnection->out_window;
         body_sz           -= pConnection->out_window;
         iov_vec[3].iov_len = pConnection->out_window;
                              pConnection->out_window = 0;

         writeData(iov_vec, false, false);

         goto loop;
         }

      iov_vec[3].iov_len       = body_sz;
      pConnection->out_window -= body_sz;

      writeData(iov_vec, false, true);
      }
}

void UHTTP2::handlerDelete(UClientImage_Base* pclient)
{
   U_TRACE(0, "UHTTP2::handlerDelete(%p)", pclient)

   U_INTERNAL_ASSERT_EQUALS(U_http_version, '2')

   uint32_t idx = (pclient - UServer_Base::vClientImage);

   U_INTERNAL_DUMP("idx = %u", idx)

   U_INTERNAL_ASSERT_MINOR(idx, UNotifier::max_connection)

   pConnection = vConnection + idx;

#ifdef DEBUG
   U_DUMP("pConnection->state = (%u, %s) pConnection->max_processed_stream_id = %u", pConnection->state, getConnectionStatusDescription(), pConnection->max_processed_stream_id)

   for (pStream = pConnection->streams, pStreamEnd = (pStream+HTTP2_MAX_CONCURRENT_STREAMS); pStream < pStreamEnd; ++pStream)
      {
      U_ASSERT(pStream->body.empty())
      U_ASSERT(pStream->headers.empty())

      U_INTERNAL_ASSERT_EQUALS(pStream->id, 0)
      U_INTERNAL_ASSERT_EQUALS(pStream->clength, 0)
      U_INTERNAL_ASSERT_EQUALS(pStream->state, STREAM_STATE_IDLE)
      }
#endif

   pConnection->state = CONN_STATE_IS_CLOSING;

   U_INTERNAL_DUMP("pclient->socket->iState = %u", pclient->socket->iState)

   if (pclient->socket->isTimeout())
      {
      nerror = NO_ERROR;

      sendGoAway();
      }

   clearHpackDynTbl(&(pConnection->idyntbl));
   clearHpackDynTbl(&(pConnection->odyntbl));
}

bool UHTTP2::initRequest()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::initRequest()")

   U_INTERNAL_ASSERT_EQUALS(U_http_version, '2')

   uint32_t sz = (UServer_Base::pClientImage - UServer_Base::vClientImage);

   U_INTERNAL_DUMP("idx = %u UNotifier::max_connection = %u", sz, UNotifier::max_connection)

   U_INTERNAL_ASSERT_MINOR(sz, UNotifier::max_connection)

   pConnection = (vConnection + sz);

   U_DUMP("pConnection->state = (%u, %s) pConnection->max_processed_stream_id = %u", pConnection->state, getConnectionStatusDescription(), pConnection->max_processed_stream_id)

   pConnection->itable.clear(); // NB: we can't clear it before because UClientImage_Base::getRequestUri() depend on it...

   pStream    =
   pStreamEnd = pConnection->streams;

   U_DUMP("pStream->state = (%u, %s)", pStream->state, getStreamStatusDescription())

   U_ASSERT(pStream->body.empty())
   U_ASSERT(pStream->headers.empty())

   U_INTERNAL_ASSERT_EQUALS(pStream->id, 0)
   U_INTERNAL_ASSERT_EQUALS(pStream->clength, 0)
   U_INTERNAL_ASSERT_EQUALS(pStream->state, STREAM_STATE_IDLE)

   if (pConnection->state != CONN_STATE_IDLE)
      {
      if (pConnection->state == CONN_STATE_OPEN) U_RETURN(true);

      pConnection->reset();
      }

   pConnection->state = CONN_STATE_OPEN;

   U_RETURN(false);
}

void UHTTP2::handlerRequest()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::handlerRequest()")

   uint32_t sz;
   const char* ptr;

   bnghttp2      =
   bsetting_ack  =
   bsetting_send = false;

   nerror                =
   hpack_errno           =
   wait_for_continuation = 0;

   if (initRequest() == false)
      {
      if (U_http2_settings_len)
         {
         bsetting_send = true;

         (void) USocketExt::write(UServer_Base::csocket, U_CONSTANT_TO_PARAM(HTTP2_CONNECTION_UPGRADE HTTP2_SETTINGS_BIN), U_SSL_TIMEOUT_MS);

         U_INTERNAL_ASSERT_MAJOR(U_http_info.startHeader, 2)
         U_INTERNAL_ASSERT_RANGE(1,UClientImage_Base::uri_offset,64)

         sz = U_http_info.startHeader - UClientImage_Base::uri_offset + U_CONSTANT_SIZE(" HTTP/1.1\r\n");

         if (sz > sizeof(UClientImage_Base::cbuffer)) goto err;

         U_INTERNAL_DUMP("UClientImage_Base::wbuffer(%u) = %V", UClientImage_Base::wbuffer->size(), UClientImage_Base::wbuffer->rep)

         if (*UClientImage_Base::wbuffer) UHTTP2::updateSetting(*UClientImage_Base::wbuffer);

         U_INTERNAL_DUMP("U_http_info.uri(%u) = %.*S U_http_method_type = %B U_http_method_num = %d",
                          U_http_info.uri_len, U_http_info.uri_len, U_http_info.uri, U_http_method_type, U_http_method_num)

         if (U_http_method_type == HTTP_OPTIONS)
            {
            uint32_t endHeader = U_http_info.endHeader;

            U_HTTP_INFO_RESET(0);

            U_http_version = '2';
            U_http_info.endHeader = endHeader;

            goto next;
            }

         /**
          * The HTTP/1.1 request that is sent prior to upgrade is assigned a stream identifier of 1 with default priority values.
          * Stream 1 is implicitly "half-closed" from the client toward the server, since the request is completed as an HTTP/1.1
          * request. After commencing the HTTP/2 connection, stream 1 is used for the response
          */

         pStream->id                          =
         pConnection->max_processed_stream_id = 1;

         pStream->state = STREAM_STATE_HALF_CLOSED;

         if ((pStream->clength = U_http_info.clength))
            {
            pStream->body.setBuffer(U_http_info.clength);

            // NB: check for 'Expect: 100-continue' (as curl does)...

            bcontinue100 = (UClientImage_Base::request->find("Expect: 100-continue", U_http_info.startHeader,
                                             U_CONSTANT_SIZE("Expect: 100-continue"),  U_http_info.endHeader - U_CONSTANT_SIZE(U_CRLF2) - U_http_info.startHeader) != U_NOT_FOUND);
            }

         U_http_info.uri = UClientImage_Base::cbuffer;

         U_MEMCPY(UClientImage_Base::cbuffer, UClientImage_Base::request->c_pointer(UClientImage_Base::uri_offset), sz);
         }

next: U_INTERNAL_ASSERT(UClientImage_Base::request->same(*UClientImage_Base::rbuffer))

      UClientImage_Base::request->clear();

      // maybe we have read more data than necessary...

      sz = UClientImage_Base::rbuffer->size();

      U_INTERNAL_ASSERT_MAJOR(sz, 0)

      if (sz > U_http_info.endHeader) UClientImage_Base::rstart = U_http_info.endHeader;
      else
         {
         // we wait for HTTP2_CONNECTION_PREFACE...

         UClientImage_Base::rbuffer->setEmptyForce();

         if (USocketExt::read(UServer_Base::csocket, *UClientImage_Base::rbuffer, U_SINGLE_READ, U_SSL_TIMEOUT_MS, UHTTP::request_read_timeout) == false) goto err;

         UClientImage_Base::rstart = 0;

         sz = UClientImage_Base::rbuffer->size();
         }

      ptr = UClientImage_Base::rbuffer->c_pointer(UClientImage_Base::rstart);

      if (u_get_unalignedp64(ptr)    != U_MULTICHAR_CONSTANT64( 'P', 'R','I',' ', '*', ' ', 'H', 'T') ||
          u_get_unalignedp64(ptr+8)  != U_MULTICHAR_CONSTANT64( 'T', 'P','/','2', '.', '0','\r','\n') ||
          u_get_unalignedp64(ptr+16) != U_MULTICHAR_CONSTANT64('\r','\n','S','M','\r','\n','\r','\n'))
         {
         goto err;
         }

      UClientImage_Base::rstart += U_CONSTANT_SIZE(HTTP2_CONNECTION_PREFACE);
      }

read_request:
   readFrame();

   U_INTERNAL_DUMP("nerror = %u", nerror)

   if (nerror == NO_ERROR)
      {
      U_INTERNAL_DUMP("bsetting_ack = %b bsetting_send = %b pStream = %p pStreamEnd = %p", bsetting_ack, bsetting_send, pStream, pStreamEnd)

      U_INTERNAL_ASSERT_RANGE(pConnection->streams,pStream,pStreamEnd)

      sz = pStream->headers.size();

      U_INTERNAL_DUMP("sz = %u", sz)

      if (sz == 0)
         {
         U_INTERNAL_ASSERT_EQUALS(pStream,    pConnection->streams)
         U_INTERNAL_ASSERT_EQUALS(pStreamEnd, pConnection->streams)

         U_INTERNAL_DUMP("U_http_info.uri_len = %u U_http2_settings_len = %u", U_http_info.uri_len, U_http2_settings_len)

         if (U_http2_settings_len)
            {
            // NB: not OPTION upgrade...

            U_INTERNAL_ASSERT(bsetting_send)

            if (bsetting_ack == false) goto read_request; // we must wait for SETTINGS ack...
      
            goto process_request;
            }

         U_INTERNAL_ASSERT_EQUALS(U_http_info.uri_len, 0)

         if (bsetting_send) goto read_request; // NB: OPTION upgrade, we need a HEADERS frame...

         UClientImage_Base::setRequestProcessed();

         return;
         }

      if (bsetting_ack == false) goto read_request; // we must wait for SETTINGS ack...

      U_INTERNAL_ASSERT(bsetting_ack)
      U_INTERNAL_ASSERT(bsetting_send)

      pStream = pConnection->streams;

loop: U_DUMP("pStream->id = %d pStream->state = (%u, %s) pStream->headers(%u) = %V pStream->clength = %u pStream->body(%u) = %V",
              pStream->id, pStream->state, getStreamStatusDescription(), sz, pStream->headers.rep, pStream->clength, pStream->body.size(), pStream->body.rep)

      if (pStream->state <= STREAM_STATE_OPEN) goto read_request;

      if ((int)pStream->id > 0)
         {
         decodeHeaders(); // parse header block

         U_INTERNAL_DUMP("nerror = %u", nerror)

         if (nerror != NO_ERROR) goto err;

process_request:
         if (pStream == pConnection->streams &&
             U_HTTP_USER_AGENT_MEMEQ("nghttp2"))
            {
            bnghttp2 = true; // NB: to avoid GOAWAY frame with error (6, FRAME_SIZE_ERROR) in writeResponse()...
            }

#     ifndef U_LOG_DISABLE
         if (sz) U_SRV_LOG_WITH_ADDR("received request (%s,id:%u,bytes:%u) %#.*S from", bnghttp2 ? "nghttp2" : "HTTP2", pStream->id, sz, sz, pStream->headers.data());
#     endif

         U_INTERNAL_DUMP("pStream->clength = %u UHTTP::limit_request_body = %u", pStream->clength, UHTTP::limit_request_body)

         if (pStream->clength > UHTTP::limit_request_body)
            {
            U_http_info.nResponseCode = HTTP_ENTITY_TOO_LARGE;

            UHTTP::setResponse();
            }
         else
            {
            U_INTERNAL_DUMP("bcontinue100 = %b", bcontinue100)

            if (bcontinue100)
               {
               bcontinue100 = false;

               U_INTERNAL_ASSERT(UServer_Base::csocket->isOpen())

               char buffer[HTTP2_FRAME_HEADER_SIZE+5] = { 0, 0, 5,
                                                          HEADERS,
                                                          FLAG_END_HEADERS,
                                                          0, 0, 0, 0,
                                                          8, 3, '1', '0', '0' };

               ptr = buffer;

               *(uint32_t*)(ptr+5) = htonl(pStream->id);

               (void) USocketExt::write(UServer_Base::csocket, buffer, sizeof(buffer), U_SSL_TIMEOUT_MS);

               goto read_request; // wait for DATA frames
               }

            if (pStream->clength != pStream->body.size())
               {
               if (pStream->body.empty()) goto read_request; // wait for DATA frames

               nerror = PROTOCOL_ERROR;
               
               goto err;
               }

            U_http_info.clength = pStream->clength;

            *UClientImage_Base::body = pStream->body;

            if (UHTTP::manageRequest() == U_PLUGIN_HANDLER_ERROR) goto err;
            }

         if (UHTTP::isHEAD()) UClientImage_Base::body->clear();

         writeResponse();

         U_INTERNAL_DUMP("nerror = %u", nerror)

         if (nerror != NO_ERROR) goto err;
         }

      pStream->id = -pStream->id;

      while (++pStream <= pStreamEnd)
         {
         if ((int)pStream->id > 0)
            {
            sz = pStream->headers.size();

            U_INTERNAL_DUMP("sz = %u", sz)

            U_INTERNAL_ASSERT_MAJOR(sz, 0)

            // ------------------------------
            // U_http_info.uri
            // ....
            // U_http_info.nResponseCode
            // ....
            // ------------------------------
            U_HTTP_INFO_RESET(0);

            U_http_version = '2';

            pConnection->itable.clear();

            goto loop;
            }
         }

      UClientImage_Base::body->clear();
      UClientImage_Base::wbuffer->clear();

#  ifdef DEBUG
      for (pStream = pConnection->streams; pStream <= pStreamEnd; ++pStream)
         {
         U_DUMP("pStream->id = %d pStream->state = (%u, %s) pStream->headers = %V pStream->clength = %u pStream->body(%u) = %V",
                 pStream->id, pStream->state, getStreamStatusDescription(), pStream->headers.rep, pStream->clength,  pStream->body.size(), pStream->body.rep)

         U_INTERNAL_ASSERT_MINOR((int)pStream->id, 0)
         }
#  endif

      goto end;
      }

err:
   if (UServer_Base::csocket->isOpen())
      {
      sendGoAway();

      UClientImage_Base::close();
      }

   U_ClientImage_state = U_PLUGIN_HANDLER_ERROR;

end:
   clear();
}

#define U_HTTP2_ENTRY(n) n: descr = #n; break

const char* UHTTP2::getFrameErrorCodeDescription(uint32_t error)
{
   U_TRACE(0, "UHTTP2::getFrameErrorCodeDescription(%u)", error)

   const char* descr;

   switch (error)
      {
      case U_HTTP2_ENTRY(NO_ERROR);             //  0: The endpoint NOT detected an error
      case U_HTTP2_ENTRY(PROTOCOL_ERROR);       //  1: The endpoint detected an unspecific protocol error. This error is for use when a more specific error code is not available
      case U_HTTP2_ENTRY(INTERNAL_ERROR);       //  2: The endpoint encountered an unexpected internal error
      case U_HTTP2_ENTRY(FLOW_CONTROL_ERROR);   //  3: The endpoint detected that its peer violated the flow-control protocol
      case U_HTTP2_ENTRY(SETTINGS_TIMEOUT);     //  4: The endpoint sent a SETTINGS frame but did not receive a response in a timely manner
      case U_HTTP2_ENTRY(STREAM_CLOSED);        //  5: The endpoint received a frame after a stream was half-closed
      case U_HTTP2_ENTRY(FRAME_SIZE_ERROR);     //  6: The endpoint received a frame with an invalid size
      case U_HTTP2_ENTRY(REFUSED_STREAM);       //  7: The endpoint refused the stream prior to performing any application processing
      case U_HTTP2_ENTRY(CANCEL);               //  8: Used by the endpoint to indicate that the stream is no longer needed
      case U_HTTP2_ENTRY(COMPRESSION_ERROR);    //  9: The endpoint is unable to maintain the header compression context for the connection
      case U_HTTP2_ENTRY(CONNECT_ERROR);        // 10: The connection established in response to a CONNECT request (Section 8.3) was reset or abnormally closed
      case U_HTTP2_ENTRY(ENHANCE_YOUR_CALM);    // 11: The endpoint detected that its peer is exhibiting a behavior that might be generating excessive load
      case U_HTTP2_ENTRY(INADEQUATE_SECURITY);  // 12: The underlying transport has properties that do not meet minimum security requirements
      case U_HTTP2_ENTRY(HTTP_1_1_REQUIRED);    // 13: The endpoint requires that HTTP/1.1 be used instead of HTTP/2
      case U_HTTP2_ENTRY(ERROR_INCOMPLETE);

      default: descr = "Error type unknown";
      }

   U_RETURN(descr);
}

__pure bool UHTTP2::isHeaderName(const char* s, uint32_t n)
{
   U_TRACE(0, "UHTTP2::isHeaderName(%.*S,%u)", n, s, n)

   unsigned char c;

   if (*s == ':')
      {
      c = *++s;

      if (c != 'a' && // :authority
          c != 'm' && // :method
          c != 'p' && // :path
          c != 's')   // :scheme|:status
         {
         U_INTERNAL_DUMP("c(%u) = %C", c, c)

         U_RETURN(false);
         }

      --n;
      }

   c = *s;

   while (n--)
      {
      if (u__isename(c) == false)
         {
         U_INTERNAL_DUMP("c(%u) = %C", c, c)

         U_RETURN(false);
         }

      c = *++s;
      }

   U_RETURN(true);
}

__pure bool UHTTP2::isHeaderValue(const char* s, uint32_t n)
{
   U_TRACE(0, "UHTTP2::isHeaderValue(%.*S,%u)", n, s, n)

   unsigned char c = *s;

   while (n--)
      {
      if ((c == '\t' || (c >= ' ' && c != 0x7f)) == false)
         {
         U_INTERNAL_DUMP("c(%u) = %C", c, c)

         U_RETURN(false);
         }

      c = *(++s);
      }

   U_RETURN(true);
}

void UHTTP2::clearHpackDynTbl(HpackDynamicTable* dyntbl)
{
   U_TRACE(0, "UHTTP2::clearHpackDynTbl(%p)", dyntbl)

   if (dyntbl->num_entries)
      {
      U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u hpack_capacity = %u hpack_max_capacity = %u",
                        dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)

      uint32_t index = dyntbl->entry_start_index;

      do {
         HpackHeaderTableEntry* entry = dyntbl->entries + index;

         delete entry->name;
         delete entry->value;

         if (++index == dyntbl->entry_capacity) index = 0;

         dyntbl->num_entries--;
         }
      while (dyntbl->num_entries != 0);

      UMemoryPool::_free(dyntbl->entries, dyntbl->entry_capacity, sizeof(HpackHeaderTableEntry));

      dyntbl->entry_capacity    =
      dyntbl->entry_start_index =
      dyntbl->hpack_size        = 0;
      dyntbl->entries           = 0;

      U_INTERNAL_DUMP("num_entries = %u entry_capacity = %u entry_start_index = %u hpack_size = %u hpack_capacity = %u hpack_max_capacity = %u",
                        dyntbl->num_entries, dyntbl->entry_capacity, dyntbl->entry_start_index, dyntbl->hpack_size, dyntbl->hpack_capacity, dyntbl->hpack_max_capacity)
      }
}

void UHTTP2::sendWindowUpdate()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::sendWindowUpdate()")

   U_INTERNAL_ASSERT(UServer_Base::csocket->isOpen())

   char buffer[HTTP2_FRAME_HEADER_SIZE+4+
               HTTP2_FRAME_HEADER_SIZE+4] = { 0, 0, 4,       // frame size
                                              WINDOW_UPDATE, // header frame
                                              FLAG_NONE,     // flags
                                              0, 0, 0, 0,    // stream id
                                              0, 0, 0, 0,
                                              0, 0, 4,       // frame size
                                              WINDOW_UPDATE, // header frame
                                              FLAG_NONE,     // flags
                                              0, 0, 0, 0,    // stream id
                                              0, 0, 0, 0 };

   char* ptr = buffer;

   *(uint32_t*)(ptr+                          9) = htonl(pConnection->inp_window);
   *(uint32_t*)(ptr+HTTP2_FRAME_HEADER_SIZE+4+5) = htonl(pStream->id);
   *(uint32_t*)(ptr+HTTP2_FRAME_HEADER_SIZE+4+9) = htonl(pConnection->inp_window);

   (void) USocketExt::write(UServer_Base::csocket, (const char*)buffer, sizeof(buffer), U_SSL_TIMEOUT_MS);
}

void UHTTP2::sendPing()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::sendPing()")

   U_INTERNAL_ASSERT(UServer_Base::csocket->isOpen())

   char buffer[HTTP2_FRAME_HEADER_SIZE+8] = { 0, 0, 8,    // frame size
                                              PING,       // ping frame
                                              FLAG_ACK,   // ack flag
                                              0, 0, 0, 0, // stream id
                                              0, 0, 0, 0, 0, 0, 0, 0 };

   char* ptr = buffer;

   *(uint32_t*)(ptr+5) = htonl(frame.stream_id);

   if (*(uint64_t*)frame.payload) U_MEMCPY(buffer+HTTP2_FRAME_HEADER_SIZE, frame.payload, 8);

   (void) USocketExt::write(UServer_Base::csocket, buffer, sizeof(buffer), U_SSL_TIMEOUT_MS);
}

void UHTTP2::sendGoAway()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::sendGoAway()")

   U_INTERNAL_ASSERT_POINTER(pConnection)
   U_INTERNAL_ASSERT(UServer_Base::csocket->isOpen())

   char buffer[HTTP2_FRAME_HEADER_SIZE+8] = { 0, 0, 8,    // frame size
                                              GOAWAY,     // header frame
                                              FLAG_NONE,  // flags
                                              0, 0, 0, 0, // stream id
                                              0, 0, 0, 0,
                                              0, 0, 0, 0 };

   char* ptr = buffer;

   *(uint32_t*)(ptr+ 9) = htonl(pConnection->max_processed_stream_id);
   *(uint32_t*)(ptr+13) = htonl(nerror);

   const char* descr = getFrameErrorCodeDescription(nerror);

   U_DEBUG("Send GOAWAY frame with error (%u, %s)", nerror, descr)

   U_SRV_LOG("send GOAWAY frame with error (%u, %s)", nerror, descr);

   pConnection->state = CONN_STATE_IS_CLOSING;

   (void) USocketExt::write(UServer_Base::csocket, buffer, sizeof(buffer), U_SSL_TIMEOUT_MS);
}

void UHTTP2::sendResetStream()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::sendResetStream()")

   U_INTERNAL_ASSERT(UServer_Base::csocket->isOpen())

   char buffer[HTTP2_FRAME_HEADER_SIZE+4] = { 0, 0, 8,    // frame size
                                              RST_STREAM, // header frame
                                              FLAG_NONE,  // flags
                                              0, 0, 0, 0, // stream id
                                              0, 0, 0, 0 };

   char* ptr = buffer;

   *(uint32_t*)(ptr+5) = htonl(frame.stream_id);
   *(uint32_t*)(ptr+9) = htonl(nerror);

   const char* descr = getFrameErrorCodeDescription(nerror);

   U_DEBUG("Send RST_STREAM frame with error (%u, %s)", nerror, descr)

   U_SRV_LOG("send RST_STREAM frame with error (%u, %s)", nerror, descr);

   pStream->state = STREAM_STATE_CLOSED;

   (void) USocketExt::write(UServer_Base::csocket, buffer, sizeof(buffer), U_SSL_TIMEOUT_MS);
}

#ifdef DEBUG
void UHTTP2::printHpackDynamicTable(const HpackDynamicTable* dyntbl, ostream& os)
{
   U_TRACE(0, "UHTTP2::printHpackDynamicTable(%p,%p)", dyntbl, &os)

   if (dyntbl->num_entries == 0) os.write(U_CONSTANT_TO_PARAM(" empty.\n"));
   else
      {
      int l;
      UString name, value;
      uint32_t len, sz = 0;
      UHTTP2::HpackHeaderTableEntry* entry;
      char str[sizeof "\n[IDX] (s = LEN) "];

      os.put('\n');

      for (uint32_t idx = 0; idx < dyntbl->num_entries; ++idx)
         {
         entry = getHpackDynTblEntry(dyntbl, idx);

          name = *(entry->name);
         value = *(entry->value);

         len = 32 + name.size() + value.size();

         l = snprintf(str, sizeof str, "\n[%3u] (s = %3u) ", idx+1, len);

         U_INTERNAL_ASSERT_EQUALS(l + 1, sizeof str)

         os.write(str, l);
         os.write(U_STRING_TO_PARAM(name));
         os.write(U_CONSTANT_TO_PARAM(": "));
         os.write(U_STRING_TO_PARAM(value));

         sz += len;
         }

      l = snprintf(str, sizeof str, "%3u\n", sz);

      os.write(U_CONSTANT_TO_PARAM("\n      Table size: "));
      os.write(str, l);
      }
}

const char* UHTTP2::getFrameTypeDescription(char type)
{
   U_TRACE(0, "UHTTP2::getFrameTypeDescription(%d)", type)

   const char* descr;

   switch (type)
      {
      case U_HTTP2_ENTRY(DATA);
      case U_HTTP2_ENTRY(HEADERS);
      case U_HTTP2_ENTRY(PRIORITY);
      case U_HTTP2_ENTRY(RST_STREAM);
      case U_HTTP2_ENTRY(SETTINGS);
      case U_HTTP2_ENTRY(PUSH_PROMISE);
      case U_HTTP2_ENTRY(PING);
      case U_HTTP2_ENTRY(GOAWAY);
      case U_HTTP2_ENTRY(WINDOW_UPDATE);
      case U_HTTP2_ENTRY(CONTINUATION);

      default: descr = "Frame type unknown";
      }

   U_RETURN(descr);
}

const char* UHTTP2::getStreamStatusDescription()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::getStreamStatusDescription()")

   const char* descr;

   switch (pStream->state)
      {
      case U_HTTP2_ENTRY(STREAM_STATE_IDLE);
      case U_HTTP2_ENTRY(STREAM_STATE_OPEN);
      case U_HTTP2_ENTRY(STREAM_STATE_HALF_CLOSED);
      case U_HTTP2_ENTRY(STREAM_STATE_CLOSED);
      case U_HTTP2_ENTRY(STREAM_STATE_RESERVED);

      default: descr = "Stream status unknown";
      }

   U_RETURN(descr);
}

const char* UHTTP2::getConnectionStatusDescription()
{
   U_TRACE_NO_PARAM(0, "UHTTP2::getConnectionStatusDescription()")

   const char* descr;

   switch (pConnection->state)
      {
      case U_HTTP2_ENTRY(CONN_STATE_IDLE);
      case U_HTTP2_ENTRY(CONN_STATE_OPEN);
      case U_HTTP2_ENTRY(CONN_STATE_IS_CLOSING);

      default: descr = "Connection status unknown";
      }

   U_RETURN(descr);
}

#undef U_HTTP2_ENTRY

U_EXPORT const char* UHTTP2::Connection::dump(bool reset) const
{
   *UObjectIO::os << "state                     " << state                   << '\n'
                  << "inp_window                " << inp_window              << '\n'
                  << "out_window                " << out_window              << '\n'
                  << "max_processed_stream_id   " << max_processed_stream_id << '\n'
                  << "itable (UHashMap<UString> " << (void*)&itable          << ')';

   if (reset)
      {
      UObjectIO::output();

      return UObjectIO::buffer_output;
      }

   return 0;
}
#endif
