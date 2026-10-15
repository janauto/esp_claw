/*
 * 收包格式、Ogg Opus 封装（RFC 7845）与流式 base64。
 *
 * 封装规则与已验证的语音中继 relay/ogg_opus.py 逐字节一致：
 * 第 0 页 OpusHead（BOS），第 1 页 OpusTags，之后每 16 包一页，最后一页置 EOS；
 * granule 以 48 kHz 计，从 pre-skip 起每包加 2880（60 ms）。
 * 纯 C，不依赖 ESP-IDF，宿主可直接编译测试。
 */
#include <string.h>

#include "vc_internal.h"

/* ---------------- 收包格式 ---------------- */

void vc_frames_iter_init(vc_frames_iter_t *it, const uint8_t *data, size_t len)
{
    it->data = data;
    it->len = data ? len : 0;
    it->pos = 0;
}

int vc_frames_next(vc_frames_iter_t *it, const uint8_t **pkt, size_t *pkt_len)
{
    if (it->pos >= it->len) {
        return 0;
    }
    if (it->len - it->pos < 2) {
        return -1; /* 长度前缀被截断 */
    }
    size_t n = (size_t)it->data[it->pos] | ((size_t)it->data[it->pos + 1] << 8);
    it->pos += 2;
    if (n == 0 || n > it->len - it->pos) {
        return -1; /* 零长度包或长度越界 */
    }
    *pkt = it->data + it->pos;
    *pkt_len = n;
    it->pos += n;
    return 1;
}

long vc_frames_count(const uint8_t *data, size_t len)
{
    vc_frames_iter_t it;
    const uint8_t *p;
    size_t n;
    long count = 0;
    int r;
    vc_frames_iter_init(&it, data, len);
    while ((r = vc_frames_next(&it, &p, &n)) == 1) {
        count++;
    }
    return r < 0 ? -1 : count;
}

/* ---------------- Ogg CRC ---------------- */

/* 预先算好的查表（多项式 0x04C11DB7，生成方法同 ogg_opus.py），常量表避免多任务初始化竞争。 */
static const uint32_t s_crc_table[256] = {
    0x00000000u, 0x04C11DB7u, 0x09823B6Eu, 0x0D4326D9u, 0x130476DCu, 0x17C56B6Bu,
    0x1A864DB2u, 0x1E475005u, 0x2608EDB8u, 0x22C9F00Fu, 0x2F8AD6D6u, 0x2B4BCB61u,
    0x350C9B64u, 0x31CD86D3u, 0x3C8EA00Au, 0x384FBDBDu, 0x4C11DB70u, 0x48D0C6C7u,
    0x4593E01Eu, 0x4152FDA9u, 0x5F15ADACu, 0x5BD4B01Bu, 0x569796C2u, 0x52568B75u,
    0x6A1936C8u, 0x6ED82B7Fu, 0x639B0DA6u, 0x675A1011u, 0x791D4014u, 0x7DDC5DA3u,
    0x709F7B7Au, 0x745E66CDu, 0x9823B6E0u, 0x9CE2AB57u, 0x91A18D8Eu, 0x95609039u,
    0x8B27C03Cu, 0x8FE6DD8Bu, 0x82A5FB52u, 0x8664E6E5u, 0xBE2B5B58u, 0xBAEA46EFu,
    0xB7A96036u, 0xB3687D81u, 0xAD2F2D84u, 0xA9EE3033u, 0xA4AD16EAu, 0xA06C0B5Du,
    0xD4326D90u, 0xD0F37027u, 0xDDB056FEu, 0xD9714B49u, 0xC7361B4Cu, 0xC3F706FBu,
    0xCEB42022u, 0xCA753D95u, 0xF23A8028u, 0xF6FB9D9Fu, 0xFBB8BB46u, 0xFF79A6F1u,
    0xE13EF6F4u, 0xE5FFEB43u, 0xE8BCCD9Au, 0xEC7DD02Du, 0x34867077u, 0x30476DC0u,
    0x3D044B19u, 0x39C556AEu, 0x278206ABu, 0x23431B1Cu, 0x2E003DC5u, 0x2AC12072u,
    0x128E9DCFu, 0x164F8078u, 0x1B0CA6A1u, 0x1FCDBB16u, 0x018AEB13u, 0x054BF6A4u,
    0x0808D07Du, 0x0CC9CDCAu, 0x7897AB07u, 0x7C56B6B0u, 0x71159069u, 0x75D48DDEu,
    0x6B93DDDBu, 0x6F52C06Cu, 0x6211E6B5u, 0x66D0FB02u, 0x5E9F46BFu, 0x5A5E5B08u,
    0x571D7DD1u, 0x53DC6066u, 0x4D9B3063u, 0x495A2DD4u, 0x44190B0Du, 0x40D816BAu,
    0xACA5C697u, 0xA864DB20u, 0xA527FDF9u, 0xA1E6E04Eu, 0xBFA1B04Bu, 0xBB60ADFCu,
    0xB6238B25u, 0xB2E29692u, 0x8AAD2B2Fu, 0x8E6C3698u, 0x832F1041u, 0x87EE0DF6u,
    0x99A95DF3u, 0x9D684044u, 0x902B669Du, 0x94EA7B2Au, 0xE0B41DE7u, 0xE4750050u,
    0xE9362689u, 0xEDF73B3Eu, 0xF3B06B3Bu, 0xF771768Cu, 0xFA325055u, 0xFEF34DE2u,
    0xC6BCF05Fu, 0xC27DEDE8u, 0xCF3ECB31u, 0xCBFFD686u, 0xD5B88683u, 0xD1799B34u,
    0xDC3ABDEDu, 0xD8FBA05Au, 0x690CE0EEu, 0x6DCDFD59u, 0x608EDB80u, 0x644FC637u,
    0x7A089632u, 0x7EC98B85u, 0x738AAD5Cu, 0x774BB0EBu, 0x4F040D56u, 0x4BC510E1u,
    0x46863638u, 0x42472B8Fu, 0x5C007B8Au, 0x58C1663Du, 0x558240E4u, 0x51435D53u,
    0x251D3B9Eu, 0x21DC2629u, 0x2C9F00F0u, 0x285E1D47u, 0x36194D42u, 0x32D850F5u,
    0x3F9B762Cu, 0x3B5A6B9Bu, 0x0315D626u, 0x07D4CB91u, 0x0A97ED48u, 0x0E56F0FFu,
    0x1011A0FAu, 0x14D0BD4Du, 0x19939B94u, 0x1D528623u, 0xF12F560Eu, 0xF5EE4BB9u,
    0xF8AD6D60u, 0xFC6C70D7u, 0xE22B20D2u, 0xE6EA3D65u, 0xEBA91BBCu, 0xEF68060Bu,
    0xD727BBB6u, 0xD3E6A601u, 0xDEA580D8u, 0xDA649D6Fu, 0xC423CD6Au, 0xC0E2D0DDu,
    0xCDA1F604u, 0xC960EBB3u, 0xBD3E8D7Eu, 0xB9FF90C9u, 0xB4BCB610u, 0xB07DABA7u,
    0xAE3AFBA2u, 0xAAFBE615u, 0xA7B8C0CCu, 0xA379DD7Bu, 0x9B3660C6u, 0x9FF77D71u,
    0x92B45BA8u, 0x9675461Fu, 0x8832161Au, 0x8CF30BADu, 0x81B02D74u, 0x857130C3u,
    0x5D8A9099u, 0x594B8D2Eu, 0x5408ABF7u, 0x50C9B640u, 0x4E8EE645u, 0x4A4FFBF2u,
    0x470CDD2Bu, 0x43CDC09Cu, 0x7B827D21u, 0x7F436096u, 0x7200464Fu, 0x76C15BF8u,
    0x68860BFDu, 0x6C47164Au, 0x61043093u, 0x65C52D24u, 0x119B4BE9u, 0x155A565Eu,
    0x18197087u, 0x1CD86D30u, 0x029F3D35u, 0x065E2082u, 0x0B1D065Bu, 0x0FDC1BECu,
    0x3793A651u, 0x3352BBE6u, 0x3E119D3Fu, 0x3AD08088u, 0x2497D08Du, 0x2056CD3Au,
    0x2D15EBE3u, 0x29D4F654u, 0xC5A92679u, 0xC1683BCEu, 0xCC2B1D17u, 0xC8EA00A0u,
    0xD6AD50A5u, 0xD26C4D12u, 0xDF2F6BCBu, 0xDBEE767Cu, 0xE3A1CBC1u, 0xE760D676u,
    0xEA23F0AFu, 0xEEE2ED18u, 0xF0A5BD1Du, 0xF464A0AAu, 0xF9278673u, 0xFDE69BC4u,
    0x89B8FD09u, 0x8D79E0BEu, 0x803AC667u, 0x84FBDBD0u, 0x9ABC8BD5u, 0x9E7D9662u,
    0x933EB0BBu, 0x97FFAD0Cu, 0xAFB010B1u, 0xAB710D06u, 0xA6322BDFu, 0xA2F33668u,
    0xBCB4666Du, 0xB8757BDAu, 0xB5365D03u, 0xB1F740B4u,
};

uint32_t vc_ogg_crc(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc = (crc << 8) ^ s_crc_table[((crc >> 24) & 0xFFu) ^ data[i]];
    }
    return crc;
}

/* ---------------- Ogg Opus 封装 ---------------- */

#define VC_OGG_PAGE_PACKETS_MAX 32 /* 每页包数上限（栈上数组大小） */

void vc_ogg_opts_default(vc_ogg_opts_t *o)
{
    o->serial = VC_OGG_SERIAL;
    o->pre_skip = VC_OPUS_PRE_SKIP;
    o->input_rate = VC_OPUS_INPUT_RATE;
    o->channels = 1;
    o->samples_per_packet = VC_SAMPLES_PER_PACKET;
    o->packets_per_page = VC_OGG_PACKETS_PER_PAGE;
    o->vendor = VC_OGG_VENDOR;
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static size_t lacing_count(size_t n)
{
    return n / 255 + 1;
}

typedef struct {
    const vc_ogg_opts_t *o;
    vc_sink_fn sink;
    void *ctx;
    bool count_only;
    size_t total;
    uint32_t seq;
} vc_mux_t;

typedef struct {
    const uint8_t *data;
    size_t len;
} vc_seg_t;

/* 写一页：页头 27 字节 + lacing 表 + 各包数据。CRC 覆盖整页（CRC 字段先置 0）。 */
static int mux_page(vc_mux_t *m, uint8_t header_type, uint64_t granule, const vc_seg_t *pk, size_t npk)
{
    uint8_t head[27 + 255];
    size_t nseg = 0;
    size_t body = 0;
    for (size_t i = 0; i < npk; i++) {
        nseg += lacing_count(pk[i].len);
        body += pk[i].len;
    }
    if (nseg > 255) {
        return -1;
    }
    m->total += 27 + nseg + body;
    if (m->count_only) {
        m->seq++;
        return 0;
    }
    memcpy(head, "OggS", 4);
    head[4] = 0; /* 版本 */
    head[5] = header_type;
    put_le64(head + 6, granule);
    put_le32(head + 14, m->o->serial);
    put_le32(head + 18, m->seq);
    put_le32(head + 22, 0); /* CRC 先置 0 */
    head[26] = (uint8_t)nseg;
    size_t k = 27;
    for (size_t i = 0; i < npk; i++) {
        size_t n = pk[i].len;
        while (n >= 255) {
            head[k++] = 255;
            n -= 255;
        }
        head[k++] = (uint8_t)n;
    }
    uint32_t crc = vc_ogg_crc(0, head, k);
    for (size_t i = 0; i < npk; i++) {
        crc = vc_ogg_crc(crc, pk[i].data, pk[i].len);
    }
    put_le32(head + 22, crc);
    if (m->sink(m->ctx, head, k)) {
        return -2;
    }
    for (size_t i = 0; i < npk; i++) {
        if (pk[i].len && m->sink(m->ctx, pk[i].data, pk[i].len)) {
            return -2;
        }
    }
    m->seq++;
    return 0;
}

static int mux_run(const uint8_t *frames, size_t frames_len, const vc_ogg_opts_t *opts, vc_mux_t *m)
{
    vc_ogg_opts_t defaults;
    if (!opts) {
        vc_ogg_opts_default(&defaults);
        opts = &defaults;
    }
    m->o = opts;
    long total = vc_frames_count(frames, frames_len);
    if (total <= 0) {
        return -1; /* 没有音频包或格式错误 */
    }
    size_t per_page = opts->packets_per_page;
    if (per_page == 0) {
        per_page = 1;
    }
    if (per_page > VC_OGG_PAGE_PACKETS_MAX) {
        per_page = VC_OGG_PAGE_PACKETS_MAX;
    }

    /* 第 0 页：OpusHead（19 字节），BOS */
    uint8_t opus_head[19];
    memcpy(opus_head, "OpusHead", 8);
    opus_head[8] = 1; /* 版本 */
    opus_head[9] = opts->channels;
    put_le16(opus_head + 10, opts->pre_skip);
    put_le32(opus_head + 12, opts->input_rate);
    put_le16(opus_head + 16, 0); /* 输出增益 */
    opus_head[18] = 0;           /* 声道映射族 */
    vc_seg_t seg = {opus_head, sizeof(opus_head)};
    int r = mux_page(m, 0x02, 0, &seg, 1);
    if (r) {
        return r;
    }

    /* 第 1 页：OpusTags = "OpusTags" + u32 厂商串长度 + 厂商串 + u32 注释数(0) */
    const char *vendor = opts->vendor ? opts->vendor : "";
    size_t vlen = strlen(vendor);
    if (vlen > 200) {
        vlen = 200;
    }
    uint8_t tags[8 + 4 + 200 + 4];
    memcpy(tags, "OpusTags", 8);
    put_le32(tags + 8, (uint32_t)vlen);
    memcpy(tags + 12, vendor, vlen);
    put_le32(tags + 12 + vlen, 0);
    seg.data = tags;
    seg.len = 16 + vlen;
    r = mux_page(m, 0x00, 0, &seg, 1);
    if (r) {
        return r;
    }

    /* 音频页 */
    vc_seg_t batch[VC_OGG_PAGE_PACKETS_MAX];
    size_t nbatch = 0;
    size_t nlacing = 0;
    uint64_t granule = opts->pre_skip;
    vc_frames_iter_t it;
    const uint8_t *p;
    size_t n;
    long index = 0;
    vc_frames_iter_init(&it, frames, frames_len);
    while (vc_frames_next(&it, &p, &n) == 1) {
        /* 防御：单包很大时 lacing 表可能放不下，先把已攒的包落成一页（正常 ≤1275 字节的包不会触发）。 */
        if (nbatch && nlacing + lacing_count(n) > 255) {
            r = mux_page(m, 0x00, granule, batch, nbatch);
            if (r) {
                return r;
            }
            nbatch = 0;
            nlacing = 0;
        }
        if (lacing_count(n) > 255) {
            return -1; /* 单包超过一页能容纳的大小 */
        }
        batch[nbatch].data = p;
        batch[nbatch].len = n;
        nbatch++;
        nlacing += lacing_count(n);
        granule += opts->samples_per_packet;
        index++;
        bool last = index == total;
        if (nbatch >= per_page || last) {
            r = mux_page(m, last ? 0x04 : 0x00, granule, batch, nbatch);
            if (r) {
                return r;
            }
            nbatch = 0;
            nlacing = 0;
        }
    }
    return 0;
}

int vc_ogg_mux(const uint8_t *frames, size_t frames_len, const vc_ogg_opts_t *o, vc_sink_fn sink, void *ctx)
{
    if (!sink) {
        return -1;
    }
    vc_mux_t m = {.sink = sink, .ctx = ctx};
    return mux_run(frames, frames_len, o, &m);
}

size_t vc_ogg_size(const uint8_t *frames, size_t frames_len, const vc_ogg_opts_t *o)
{
    vc_mux_t m = {.count_only = true};
    if (mux_run(frames, frames_len, o, &m) != 0) {
        return 0;
    }
    return m.total;
}

/* ---------------- 流式 base64 ---------------- */

static const char k_b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void vc_b64_init(vc_b64_t *b, char *out, size_t cap)
{
    memset(b, 0, sizeof(*b));
    b->out = out;
    b->out_cap = cap;
}

static void b64_emit(vc_b64_t *b, const uint8_t in[3], size_t n)
{
    if (b->overflow || b->out_pos + 4 > b->out_cap) {
        b->overflow = true;
        return;
    }
    uint32_t v = ((uint32_t)in[0] << 16) | ((n > 1 ? (uint32_t)in[1] : 0) << 8) | (n > 2 ? in[2] : 0);
    char *o = b->out + b->out_pos;
    o[0] = k_b64[(v >> 18) & 63];
    o[1] = k_b64[(v >> 12) & 63];
    o[2] = n > 1 ? k_b64[(v >> 6) & 63] : '=';
    o[3] = n > 2 ? k_b64[v & 63] : '=';
    b->out_pos += 4;
}

void vc_b64_update(vc_b64_t *b, const uint8_t *data, size_t len)
{
    size_t i = 0;
    /* 先补齐上次剩下的 1–2 字节 */
    while (b->carry_n && b->carry_n < 3 && i < len) {
        b->carry[b->carry_n++] = data[i++];
    }
    if (b->carry_n == 3) {
        b64_emit(b, b->carry, 3);
        b->carry_n = 0;
    }
    /* 整组直接编码 */
    while (len - i >= 3) {
        b64_emit(b, data + i, 3);
        i += 3;
    }
    /* 余下 0–2 字节留到下次 */
    while (i < len) {
        b->carry[b->carry_n++] = data[i++];
    }
}

size_t vc_b64_final(vc_b64_t *b)
{
    if (b->carry_n) {
        uint8_t tail[3] = {0, 0, 0};
        memcpy(tail, b->carry, b->carry_n);
        b64_emit(b, tail, b->carry_n);
        b->carry_n = 0;
    }
    return b->overflow ? (size_t)-1 : b->out_pos;
}
