/*
 * voice_cloud 宿主测试（clang + ASan/UBSan，由 run_tests.py 编译运行）。
 *
 * 不带参数：跑全部单元测试（纯逻辑 + 用假传输驱动的流水线 + 用 pthread 桩跑的公开 API）。
 * 带参数（供 run_tests.py 交叉核对）：
 *   --mux <in.frames> <out.ogg> [vendor]   用 C 封装写出 Ogg
 *   --crc-check <file.ogg>                 用 C 的 CRC 逐页复算并与文件里的值比较
 *   --asr-body <in.frames> <out.json>      写出完整 ASR 请求体
 *   --user-strings                         打印所有给用户看的固定中文（检查字库覆盖）
 *   --clean <text> [max]                   打印清洗结果
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "freertos/idf_additions.h"
#include "nvs.h"
#include "vc_internal.h"

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        const char *a_ = (a), *b_ = (b);                                               \
        g_checks++;                                                                    \
        if (!a_ || !b_ || strcmp(a_, b_) != 0) {                                       \
            g_failures++;                                                              \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,       \
                    a_ ? a_ : "(null)", b_ ? b_ : "(null)");                           \
        }                                                                              \
    } while (0)

/* ---------------- 通用小工具 ---------------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_put(buf_t *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 64;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static int buf_sink(void *ctx, const uint8_t *data, size_t len)
{
    buf_put(ctx, data, len);
    return 0;
}

/* 与 test_relay.fake_packets 相同的假包：0x48 + ((i*7+j)&0xFF)，长度 41+i */
static buf_t fake_frames(int n)
{
    buf_t b = {0};
    for (int i = 0; i < n; i++) {
        size_t len = 1 + 40 + (size_t)i;
        uint8_t hdr[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
        buf_put(&b, hdr, 2);
        uint8_t toc = 0x48;
        buf_put(&b, &toc, 1);
        for (int j = 0; j < 40 + i; j++) {
            uint8_t v = (uint8_t)((i * 7 + j) & 0xFF);
            buf_put(&b, &v, 1);
        }
    }
    return b;
}

static void frames_add(buf_t *b, const uint8_t *pkt, size_t len)
{
    uint8_t hdr[2] = {(uint8_t)len, (uint8_t)(len >> 8)};
    buf_put(b, hdr, 2);
    buf_put(b, pkt, len);
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static buf_t b64_decode(const char *s, size_t n)
{
    buf_t out = {0};
    for (size_t i = 0; i + 3 < n; i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; k++) {
            if (s[i + k] == '=') {
                v[k] = 0;
                pad++;
            } else {
                v[k] = b64_val(s[i + k]);
            }
        }
        uint32_t x = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        uint8_t t[3] = {(uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x};
        buf_put(&out, t, (size_t)(3 - pad));
    }
    return out;
}

static uint32_t crc_bitwise(const uint8_t *p, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
        }
    }
    return crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p)
{
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

typedef struct {
    int pages;
    uint8_t types[64];
    uint64_t granules[64];
    uint32_t seqs[64];
    int crc_bad;
    int max_lacing;
    buf_t packets; /* 还原出的音频包，[u16 长度][数据] */
    buf_t head;
    buf_t tags;
} ogg_info_t;

/* 解析 Ogg：校验捕获字、CRC，还原所有包（跳过 OpusHead/OpusTags） */
static int ogg_parse(const uint8_t *d, size_t n, ogg_info_t *info)
{
    memset(info, 0, sizeof(*info));
    size_t pos = 0;
    buf_t partial = {0};
    int idx = 0;
    while (pos < n) {
        if (n - pos < 27 || memcmp(d + pos, "OggS", 4) != 0 || d[pos + 4] != 0) {
            free(partial.data);
            return -1;
        }
        int nseg = d[pos + 26];
        size_t body = pos + 27 + (size_t)nseg;
        size_t total = 0;
        for (int i = 0; i < nseg; i++) {
            total += d[pos + 27 + (size_t)i];
        }
        if (body + total > n) {
            free(partial.data);
            return -1;
        }
        uint8_t *copy = malloc(body + total - pos);
        memcpy(copy, d + pos, body + total - pos);
        uint32_t stored = le32(copy + 22);
        memset(copy + 22, 0, 4);
        if (crc_bitwise(copy, body + total - pos) != stored || vc_ogg_crc(0, copy, body + total - pos) != stored) {
            info->crc_bad++;
        }
        free(copy);
        if (info->pages < 64) {
            info->types[info->pages] = d[pos + 5];
            info->granules[info->pages] = le64(d + pos + 6);
            info->seqs[info->pages] = le32(d + pos + 18);
        }
        if (nseg > info->max_lacing) {
            info->max_lacing = nseg;
        }
        info->pages++;
        size_t off = body;
        for (int i = 0; i < nseg; i++) {
            uint8_t v = d[pos + 27 + (size_t)i];
            buf_put(&partial, d + off, v);
            off += v;
            if (v < 255) {
                if (idx == 0) {
                    buf_put(&info->head, partial.data, partial.len);
                } else if (idx == 1) {
                    buf_put(&info->tags, partial.data, partial.len);
                } else {
                    frames_add(&info->packets, partial.data, partial.len);
                }
                idx++;
                partial.len = 0;
            }
        }
        pos = body + total;
    }
    free(partial.data);
    return 0;
}

static void ogg_info_free(ogg_info_t *info)
{
    free(info->packets.data);
    free(info->head.data);
    free(info->tags.data);
}

static buf_t read_file(const char *path)
{
    buf_t b = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        return b;
    }
    uint8_t tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) {
        buf_put(&b, tmp, n);
    }
    fclose(f);
    return b;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    return w == len ? 0 : -1;
}

/* ---------------- 纯逻辑测试 ---------------- */

static void test_base64(void)
{
    static const char *in[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *out[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        char o[16] = {0};
        vc_b64_t b;
        vc_b64_init(&b, o, sizeof(o));
        vc_b64_update(&b, (const uint8_t *)in[i], strlen(in[i]));
        size_t n = vc_b64_final(&b);
        CHECK(n == strlen(out[i]));
        CHECK(memcmp(o, out[i], n) == 0);
        CHECK(vc_b64_len(strlen(in[i])) == strlen(out[i]));
    }
    /* 任意分段流式编码与一次编码结果相同 */
    uint8_t data[301];
    for (int i = 0; i < 301; i++) {
        data[i] = (uint8_t)(i * 37 + 11);
    }
    for (size_t len = 0; len <= 301; len += 7) {
        char one[512], many[512];
        vc_b64_t a, b;
        vc_b64_init(&a, one, sizeof(one));
        vc_b64_update(&a, data, len);
        size_t na = vc_b64_final(&a);
        vc_b64_init(&b, many, sizeof(many));
        size_t pos = 0, step = 1;
        while (pos < len) {
            size_t k = step > len - pos ? len - pos : step;
            vc_b64_update(&b, data + pos, k);
            pos += k;
            step = step % 5 + 1;
        }
        size_t nb = vc_b64_final(&b);
        CHECK(na == nb && na == vc_b64_len(len));
        CHECK(memcmp(one, many, na) == 0);
        buf_t back = b64_decode(one, na);
        CHECK(back.len == len && (len == 0 || memcmp(back.data, data, len) == 0));
        free(back.data);
    }
    /* 输出缓冲不够时报溢出 */
    char small[7];
    vc_b64_t s;
    vc_b64_init(&s, small, sizeof(small));
    vc_b64_update(&s, data, 6);
    CHECK(vc_b64_final(&s) == (size_t)-1);
}

static void test_frames_iter(void)
{
    buf_t f = fake_frames(5);
    CHECK(vc_frames_count(f.data, f.len) == 5);
    vc_frames_iter_t it;
    const uint8_t *p;
    size_t n;
    vc_frames_iter_init(&it, f.data, f.len);
    CHECK(vc_frames_next(&it, &p, &n) == 1 && n == 41 && p[0] == 0x48);
    /* 截断的长度前缀、零长度、长度越界都要报错 */
    uint8_t trunc[] = {0x05};
    CHECK(vc_frames_count(trunc, sizeof(trunc)) == -1);
    uint8_t zero[] = {0x00, 0x00};
    CHECK(vc_frames_count(zero, sizeof(zero)) == -1);
    uint8_t over[] = {0x09, 0x00, 0x48};
    CHECK(vc_frames_count(over, sizeof(over)) == -1);
    CHECK(vc_frames_count(NULL, 0) == 0);
    free(f.data);
}

static void test_ogg_structure(void)
{
    buf_t f = fake_frames(40);
    buf_t ogg = {0};
    vc_ogg_opts_t o;
    vc_ogg_opts_default(&o);
    CHECK(vc_ogg_mux(f.data, f.len, &o, buf_sink, &ogg) == 0);
    CHECK(vc_ogg_size(f.data, f.len, &o) == ogg.len);
    ogg_info_t info;
    CHECK(ogg_parse(ogg.data, ogg.len, &info) == 0);
    CHECK(info.crc_bad == 0);
    CHECK(info.pages == 5); /* 头、标签、16+16+8 包 */
    uint8_t types[] = {0x02, 0x00, 0x00, 0x00, 0x04};
    uint64_t gran[] = {0, 0, 312 + 16 * 2880, 312 + 32 * 2880, 312 + 40 * 2880};
    for (int i = 0; i < 5; i++) {
        CHECK(info.types[i] == types[i]);
        CHECK(info.granules[i] == gran[i]);
        CHECK(info.seqs[i] == (uint32_t)i);
    }
    CHECK(le32(ogg.data + 14) == VC_OGG_SERIAL);
    /* OpusHead：版本 1、单声道、pre-skip 312、16000 Hz、增益 0、映射 0 */
    CHECK(info.head.len == 19 && memcmp(info.head.data, "OpusHead", 8) == 0);
    CHECK(info.head.data[8] == 1 && info.head.data[9] == 1);
    CHECK((info.head.data[10] | (info.head.data[11] << 8)) == 312);
    CHECK(le32(info.head.data + 12) == 16000);
    CHECK(info.head.data[16] == 0 && info.head.data[17] == 0 && info.head.data[18] == 0);
    CHECK(info.tags.len == 16 + strlen(VC_OGG_VENDOR) && memcmp(info.tags.data, "OpusTags", 8) == 0);
    CHECK(info.packets.len == f.len && memcmp(info.packets.data, f.data, f.len) == 0);
    ogg_info_free(&info);
    free(ogg.data);

    /* 零包不能封装 */
    CHECK(vc_ogg_size(NULL, 0, &o) == 0);
    buf_t empty = {0};
    CHECK(vc_ogg_mux(NULL, 0, &o, buf_sink, &empty) == -1);
    free(f.data);
}

static void test_ogg_lacing(void)
{
    /* 600 字节 → 255,255,90；正好 255 字节 → 255,0 */
    buf_t f = {0};
    uint8_t big[600];
    memset(big, 0, sizeof(big));
    big[0] = 0x48;
    frames_add(&f, big, 600);
    frames_add(&f, big, 255);
    buf_t ogg = {0};
    CHECK(vc_ogg_mux(f.data, f.len, NULL, buf_sink, &ogg) == 0);
    ogg_info_t info;
    CHECK(ogg_parse(ogg.data, ogg.len, &info) == 0 && info.crc_bad == 0);
    CHECK(info.pages == 3 && info.max_lacing == 5);
    CHECK(info.packets.len == f.len && memcmp(info.packets.data, f.data, f.len) == 0);
    ogg_info_free(&info);
    free(ogg.data);
    free(f.data);

    /* 防御路径：每包 4000 字节（16 个 lacing），16 包一页放不下，必须拆页且仍可还原 */
    buf_t g = {0};
    uint8_t *huge = calloc(1, 4000);
    for (int i = 0; i < 17; i++) {
        huge[0] = (uint8_t)i;
        frames_add(&g, huge, 4000);
    }
    buf_t ogg2 = {0};
    CHECK(vc_ogg_mux(g.data, g.len, NULL, buf_sink, &ogg2) == 0);
    CHECK(vc_ogg_size(g.data, g.len, NULL) == ogg2.len);
    ogg_info_t info2;
    CHECK(ogg_parse(ogg2.data, ogg2.len, &info2) == 0 && info2.crc_bad == 0);
    CHECK(info2.max_lacing <= 255);
    CHECK(info2.types[info2.pages - 1] == 0x04);
    CHECK(info2.granules[info2.pages - 1] == 312 + 17 * 2880);
    CHECK(info2.packets.len == g.len && memcmp(info2.packets.data, g.data, g.len) == 0);
    ogg_info_free(&info2);
    free(ogg2.data);
    free(g.data);
    free(huge);
}

static void test_crc_table(void)
{
    uint8_t data[1000];
    for (int i = 0; i < 1000; i++) {
        data[i] = (uint8_t)(i * 131 + 7);
    }
    CHECK(vc_ogg_crc(0, data, sizeof(data)) == crc_bitwise(data, sizeof(data)));
    /* 分段累计等于一次计算 */
    uint32_t c = vc_ogg_crc(0, data, 333);
    c = vc_ogg_crc(c, data + 333, 667);
    CHECK(c == crc_bitwise(data, sizeof(data)));
    CHECK(vc_ogg_crc(0, (const uint8_t *)"", 0) == 0);
}

static void test_asr_body(void)
{
    buf_t f = fake_frames(12);
    size_t len = 0, ogg_len = 0;
    char *body = vc_asr_build_body(f.data, f.len, "qwen3-asr-flash", VC_ASR_CONTEXT, &len, &ogg_len);
    CHECK(body != NULL);
    CHECK(len == strlen(body));
    cJSON *root = cJSON_Parse(body);
    CHECK(root != NULL);
    CHECK_STR(cJSON_GetObjectItem(root, "model")->valuestring, "qwen3-asr-flash");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "stream")));
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    CHECK(cJSON_GetArraySize(msgs) == 2);
    cJSON *sys = cJSON_GetArrayItem(msgs, 0);
    CHECK_STR(cJSON_GetObjectItem(sys, "role")->valuestring, "system");
    cJSON *part = cJSON_GetArrayItem(cJSON_GetObjectItem(sys, "content"), 0);
    CHECK_STR(cJSON_GetObjectItem(part, "type")->valuestring, "text");
    CHECK_STR(cJSON_GetObjectItem(part, "text")->valuestring, VC_ASR_CONTEXT);
    CHECK(strstr(VC_ASR_CONTEXT, "背夹") != NULL);
    cJSON *user = cJSON_GetArrayItem(msgs, 1);
    CHECK_STR(cJSON_GetObjectItem(user, "role")->valuestring, "user");
    cJSON *audio = cJSON_GetArrayItem(cJSON_GetObjectItem(user, "content"), 0);
    CHECK_STR(cJSON_GetObjectItem(audio, "type")->valuestring, "input_audio");
    const char *data = cJSON_GetObjectItem(cJSON_GetObjectItem(audio, "input_audio"), "data")->valuestring;
    CHECK(strncmp(data, "data:audio/ogg;base64,", 22) == 0);
    buf_t decoded = b64_decode(data + 22, strlen(data + 22));
    buf_t ogg = {0};
    vc_ogg_mux(f.data, f.len, NULL, buf_sink, &ogg);
    CHECK(decoded.len == ogg.len && ogg.len == ogg_len && memcmp(decoded.data, ogg.data, ogg.len) == 0);
    cJSON *opts = cJSON_GetObjectItem(root, "asr_options");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(opts, "enable_itn")));
    CHECK_STR(cJSON_GetObjectItem(opts, "language")->valuestring, "zh");
    cJSON_Delete(root);
    free(decoded.data);
    free(ogg.data);
    vc_big_free(body);

    /* 不带热词：只有 user 一条消息 */
    body = vc_asr_build_body(f.data, f.len, "qwen3-asr-flash", "", &len, NULL);
    root = cJSON_Parse(body);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "messages")) == 1);
    cJSON_Delete(root);
    vc_big_free(body);
    /* 没有包时不生成 */
    CHECK(vc_asr_build_body(NULL, 0, "m", "", &len, NULL) == NULL);
    free(f.data);
}

static void test_asr_parse(void)
{
    char *t = NULL;
    const char *a = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"  背夹，你好。\\n \"}}]}";
    CHECK(vc_asr_parse(a, strlen(a), &t) == 0);
    CHECK_STR(t, "背夹，你好。");
    free(t);
    const char *b = "{\"choices\":[{\"message\":{\"content\":[{\"text\":\"背夹\"},{\"type\":\"x\"},{\"text\":\"你好\"}]}}]}";
    CHECK(vc_asr_parse(b, strlen(b), &t) == 0);
    CHECK_STR(t, "背夹你好");
    free(t);
    const char *c = "{\"choices\":[{\"message\":{\"content\":null}}]}";
    CHECK(vc_asr_parse(c, strlen(c), &t) == 0);
    CHECK_STR(t, "");
    free(t);
    const char *bad[] = {"{\"error\":{\"message\":\"x\"}}", "{\"choices\":[]}", "not json", "{\"choices\":[{\"message\":{\"content\":5}}]}"};
    for (int i = 0; i < 4; i++) {
        CHECK(vc_asr_parse(bad[i], strlen(bad[i]), &t) == -1 && t == NULL);
    }
    CHECK(vc_asr_parse(NULL, 0, &t) == -1);
}

static void test_urls(void)
{
    char out[VC_URL_MAX];
    vc_flavor_t fl;
    CHECK(vc_asr_url("", out, sizeof(out)) == 0);
    CHECK_STR(out, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(vc_asr_url("https://ws-abc.cn-beijing.maas.aliyuncs.com/", out, sizeof(out)) == 0);
    CHECK_STR(out, "https://ws-abc.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(vc_asr_url("https://dashscope.aliyuncs.com/compatible-mode/v1", out, sizeof(out)) == 0);
    CHECK_STR(out, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(vc_asr_url(" https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions/ ", out, sizeof(out)) == 0);
    CHECK_STR(out, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    const char *bad[] = {"http://dashscope.aliyuncs.com", "https://user:pw@x.com", "https://x.com?key=1", "https://",
                         "https://x .com", "ftp://x.com", "https://x.com/#frag", "https:///path", "https://x\"y.com"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(vc_asr_url(bad[i], out, sizeof(out)) == -1);
    }

    CHECK(vc_chat_url("", "", "deepseek-flash", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://api.deepseek.com/chat/completions");
    CHECK(fl == VC_FLAVOR_DEEPSEEK);
    CHECK(vc_chat_url("https://api.deepseek.com/v1/", "", "deepseek-flash", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://api.deepseek.com/v1/chat/completions");
    CHECK(vc_chat_url("https://api.deepseek.com/chat/completions", "", "x", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://api.deepseek.com/chat/completions");
    CHECK(fl == VC_FLAVOR_DEEPSEEK);
    CHECK(vc_chat_url("https://dashscope.aliyuncs.com", "", "qwen-plus", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(fl == VC_FLAVOR_DASHSCOPE);
    CHECK(vc_chat_url("https://ws-abc.cn-beijing.maas.aliyuncs.com", "https://ws-abc.cn-beijing.maas.aliyuncs.com",
                      "qwen-plus", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://ws-abc.cn-beijing.maas.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(fl == VC_FLAVOR_DASHSCOPE);
    CHECK(vc_chat_url("https://dashscope.aliyuncs.com/compatible-mode/v1", "", "qwen-plus", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK(vc_chat_url("https://api.openai.com/v1", "", "gpt-4o-mini", out, sizeof(out), &fl) == 0);
    CHECK_STR(out, "https://api.openai.com/v1/chat/completions");
    CHECK(fl == VC_FLAVOR_OPENAI);
    CHECK(vc_chat_url("https://api.example.com/v1", "", "deepseek-chat", out, sizeof(out), &fl) == 0);
    CHECK(fl == VC_FLAVOR_DEEPSEEK);
    CHECK(vc_chat_url("http://api.deepseek.com", "", "x", out, sizeof(out), &fl) == -1);
    char host[VC_HOST_MAX];
    CHECK(vc_url_host("https://Api.DeepSeek.com:8443/x", host, sizeof(host)) == 0);
    CHECK_STR(host, "api.deepseek.com:8443");
    CHECK(vc_url_host("http://x", host, sizeof(host)) == -1);
    CHECK(vc_key_valid("sk-abc_123") && !vc_key_valid("") && !vc_key_valid("sk abc") && !vc_key_valid("sk\n"));
    CHECK(vc_model_valid("qwen3-asr-flash") && !vc_model_valid("a b") && !vc_model_valid("a\"b"));
}

static void test_chat_body(void)
{
    char *sys = vc_system_prompt("2026年10月16日 星期五 09:30");
    CHECK(strstr(sys, "80 个字") != NULL);
    CHECK(strstr(sys, "当前时间：2026年10月16日 星期五 09:30。") != NULL);
    vc_turn_t hist[2] = {{"问一", "答一"}, {"问二", "答二"}};
    cJSON *msgs = vc_chat_messages(sys, hist, 2, "把亮度调到50%");
    free(sys);
    CHECK(cJSON_GetArraySize(msgs) == 6);
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 0), "role")->valuestring, "system");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 3), "content")->valuestring, "问二");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 4), "role")->valuestring, "assistant");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetArrayItem(msgs, 5), "content")->valuestring, "把亮度调到50%");
    cJSON *tools = cJSON_Parse(voice_cloud_default_tools_json());
    CHECK(cJSON_GetArraySize(tools) == 4);

    size_t len = 0;
    char *body = vc_chat_build_body("deepseek-flash", VC_FLAVOR_DEEPSEEK, msgs, tools, "auto", &len);
    cJSON *root = cJSON_Parse(body);
    CHECK(len == strlen(body));
    CHECK_STR(cJSON_GetObjectItem(root, "model")->valuestring, "deepseek-flash");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "stream")));
    CHECK(cJSON_GetObjectItem(root, "max_tokens")->valueint == VC_CHAT_MAX_TOKENS);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "messages")) == 6);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(root, "tools")) == 4);
    CHECK_STR(cJSON_GetObjectItem(root, "tool_choice")->valuestring, "auto");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(root, "thinking"), "type")->valuestring, "disabled");
    CHECK(cJSON_GetObjectItem(root, "enable_thinking") == NULL);
    cJSON_Delete(root);
    cJSON_free(body);

    body = vc_chat_build_body("qwen-plus", VC_FLAVOR_DASHSCOPE, msgs, tools, "none", &len);
    root = cJSON_Parse(body);
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(root, "enable_thinking")));
    CHECK(cJSON_GetObjectItem(root, "thinking") == NULL);
    CHECK_STR(cJSON_GetObjectItem(root, "tool_choice")->valuestring, "none");
    cJSON_Delete(root);
    cJSON_free(body);

    body = vc_chat_build_body("gpt", VC_FLAVOR_OPENAI, msgs, tools, NULL, &len);
    root = cJSON_Parse(body);
    CHECK(cJSON_GetObjectItem(root, "tools") == NULL && cJSON_GetObjectItem(root, "tool_choice") == NULL);
    CHECK(cJSON_GetObjectItem(root, "thinking") == NULL && cJSON_GetObjectItem(root, "enable_thinking") == NULL);
    cJSON_Delete(root);
    cJSON_free(body);
    /* messages/tools 只被引用，仍然完好 */
    CHECK(cJSON_GetArraySize(msgs) == 6 && cJSON_GetArraySize(tools) == 4);
    cJSON_Delete(msgs);
    cJSON_Delete(tools);

    sys = vc_system_prompt(NULL);
    CHECK(strstr(sys, "当前时间") == NULL);
    free(sys);
}

static void test_tools_normalize(void)
{
    char *n = vc_tools_normalize(voice_cloud_default_tools_json());
    CHECK(n != NULL);
    cJSON *arr = cJSON_Parse(n);
    CHECK(vc_tools_has(arr, "open_app") && vc_tools_has(arr, "set_brightness") && vc_tools_has(arr, "set_volume") &&
          vc_tools_has(arr, "set_mode") && !vc_tools_has(arr, "format_disk"));
    cJSON_Delete(arr);
    free(n);
    const char *bad[] = {
        "{bad",
        "{}",
        "[]",
        "[{\"type\":\"function\",\"function\":{}}]",
        "[{\"type\":\"function\",\"function\":{\"name\":\"a b\"}}]",
        "[{\"type\":\"retrieval\",\"function\":{\"name\":\"a\"}}]",
        "[{\"type\":\"function\",\"function\":{\"name\":\"a\"}},{\"type\":\"function\",\"function\":{\"name\":\"a\"}}]",
        "[{\"type\":\"function\",\"function\":{\"name\":\"a\",\"parameters\":5}}]",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(vc_tools_normalize(bad[i]) == NULL);
    }
    /* 超过 16 个工具 */
    char many[4096] = "[";
    for (int i = 0; i < 17; i++) {
        char item[96];
        snprintf(item, sizeof(item), "%s{\"type\":\"function\",\"function\":{\"name\":\"t%d\"}}", i ? "," : "", i);
        strcat(many, item);
    }
    strcat(many, "]");
    CHECK(vc_tools_normalize(many) == NULL);
    CHECK(vc_tools_normalize(NULL) == NULL);
}

static void clean_case(const char *in, const char *expect, int line)
{
    char out[VC_REPLY_MAX_BYTES + 1];
    vc_text_clean(in, out, sizeof(out), VC_REPLY_MAX_BYTES);
    g_checks++;
    if (strcmp(out, expect) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL clean (line %d): \"%s\" -> \"%s\" (expect \"%s\")\n", line, in, out, expect);
    }
}

static void test_text_clean(void)
{
    clean_case("**你好**，我是*背夹*助手！", "你好，我是背夹助手！", __LINE__);
    clean_case("Hello!", "Hello！", __LINE__);
    clean_case("温度 25°C，湿度 60%", "温度 25度，湿度 60%", __LINE__);
    clean_case("今天 30℃", "今天 30度", __LINE__);
    clean_case("从北京→上海", "从北京->上海", __LINE__);
    clean_case("好的😀👍", "好的", __LINE__);
    clean_case("已开启 ✅", "已开启", __LINE__);
    clean_case("# 标题\n- 第一项\n- 第二项", "标题，第一项，第二项", __LINE__);
    clean_case("1. 打开设置\n2. 点击显示", "打开设置，点击显示", __LINE__);
    clean_case("1、打开设置\n2、点击显示", "打开设置，点击显示", __LINE__);
    clean_case("亮度提高到1.5倍", "亮度提高到1.5倍", __LINE__);
    clean_case("```c\nint x;\n```\n完成", "完成", __LINE__);
    clean_case("[官网](https://x.com)见此", "官网见此", __LINE__);
    clean_case("![图](u)", "图", __LINE__);
    clean_case("a^2", "a2", __LINE__);
    clean_case("５０％", "50%", __LINE__);
    clean_case("（括号）“引号”…·—", "（括号）“引号”…·—", __LINE__);
    clean_case("价格¥100", "价格￥100", __LINE__);
    clean_case("Line1\nLine2", "Line1 Line2", __LINE__);
    clean_case("完成。\n下一步", "完成。下一步", __LINE__);
    clean_case("~~删除~~保留", "删除保留", __LINE__);
    clean_case("| 名称 | 值 |\n|---|---|\n| a | b |", "名称 值，a b", __LINE__);
    clean_case("\xff\xfe好", "好", __LINE__);
    clean_case("a\xe2\x80\x8b" "b", "ab", __LINE__);
    clean_case("①打开②关闭", "1.打开2.关闭", __LINE__);
    clean_case("a\x01" "b\tc", "ab c", __LINE__);
    clean_case("> 引用内容", "引用内容", __LINE__);
    clean_case("---\n内容\n***", "内容", __LINE__);
    clean_case("   多   空格   ", "多 空格", __LINE__);
    clean_case("3×4=12，10÷2", "3x4=12，10/2", __LINE__);
    clean_case("", "", __LINE__);
    clean_case("😀", "", __LINE__);

    /* 截断：无句末时硬截并补“…”，总长 ≤ 300 且是合法 UTF-8 */
    char longs[1300] = "";
    for (int i = 0; i < 200; i++) {
        strcat(longs, "好");
    }
    char out[VC_REPLY_MAX_BYTES + 1];
    size_t n = vc_text_clean(longs, out, sizeof(out), VC_REPLY_MAX_BYTES);
    CHECK(n == 300 && strlen(out) == 300 && vc_utf8_valid(out, n));
    CHECK(memcmp(out + 297, "…", 3) == 0);
    /* 有足够靠后的句末：在句末截断，不加省略号 */
    char sent[1300] = "";
    for (int i = 0; i < 30; i++) {
        strcat(sent, "这是一句话。");
    }
    n = vc_text_clean(sent, out, sizeof(out), VC_REPLY_MAX_BYTES);
    CHECK(n <= 300 && n >= 180 && memcmp(out + n - 3, "。", 3) == 0);
    /* 逐个上限检查：任何 max 下输出都合法且不超长 */
    const char *mixed = "背夹Hello，世界！😀ABC→，这是一段用于截断测试的文字，含 English 与中文。";
    for (size_t max = 0; max < 140; max++) {
        char o2[160];
        size_t m = vc_text_clean(mixed, o2, max + 1, max);
        CHECK(m <= max && strlen(o2) == m && vc_utf8_valid(o2, m));
    }
    /* UTF-8 切点 */
    const char *zh = "中文";
    CHECK(vc_utf8_cut(zh, 6, 4) == 3 && vc_utf8_cut(zh, 6, 2) == 0 && vc_utf8_cut(zh, 6, 6) == 6);
    CHECK(!vc_utf8_valid("\xc0\x80", 2) && !vc_utf8_valid("\xed\xa0\x80", 3) && vc_utf8_valid("中", 3));
}

static void test_errors(void)
{
    char d[40];
    CHECK_STR(vc_err_http(VC_STAGE_ASR, 401, d, sizeof(d)), "语音识别服务鉴权失败，请检查密钥");
    CHECK_STR(d, "asr_http_401");
    CHECK_STR(vc_err_http(VC_STAGE_CHAT, 403, d, sizeof(d)), "对话服务鉴权失败，请检查密钥");
    CHECK_STR(vc_err_http(VC_STAGE_CHAT, 503, d, sizeof(d)), "服务暂时不可用，请稍后再试");
    CHECK_STR(d, "chat_http_503");
    CHECK_STR(vc_err_http(VC_STAGE_CHAT, 429, d, sizeof(d)), "服务太忙了，请稍后再试");
    CHECK_STR(vc_err_http(VC_STAGE_ASR, 404, d, sizeof(d)), "服务地址或模型名不对，请检查配置");
    CHECK_STR(vc_err_http(VC_STAGE_CHAT, 402, d, sizeof(d)), "对话服务账户余额不足");
    CHECK_STR(vc_err_http(VC_STAGE_ASR, 400, d, sizeof(d)), "语音识别请求被拒绝，请再说一次");
    CHECK_STR(vc_err_http(VC_STAGE_CHAT, 418, d, sizeof(d)), "服务出错，请稍后再试");
    CHECK_STR(d, "chat_http_418");
    CHECK(vc_asr_budget_ms(0) == 15000 && vc_asr_budget_ms(500) == 18750 && vc_asr_budget_ms(2000) == 30000);
}

/* ---------------- 假传输 ---------------- */

typedef struct {
    vc_net_t net;
    int status;
    const char *body;
    int delay_ms;
} step_t;

#define MAX_REQ 32
static struct {
    pthread_mutex_t mu;
    step_t steps[MAX_REQ];
    int n, pos;
    char *url[MAX_REQ];
    char *body[MAX_REQ];
    char *key[MAX_REQ];
    int timeout[MAX_REQ];
    int nreq;
    bool virtual_time;
    int64_t now;
    atomic_bool *cancel_on_req; /* 第 cancel_at 次请求时置位，模拟用户取消 */
    int cancel_at;
} F = {.mu = PTHREAD_MUTEX_INITIALIZER};

static void fake_reset(void)
{
    pthread_mutex_lock(&F.mu);
    for (int i = 0; i < F.nreq; i++) {
        free(F.url[i]);
        free(F.body[i]);
        free(F.key[i]);
    }
    F.n = F.pos = F.nreq = 0;
    F.now = 1000000;
    F.cancel_on_req = NULL;
    F.cancel_at = -1;
    pthread_mutex_unlock(&F.mu);
}

static void fake_add(vc_net_t net, int status, const char *body, int delay_ms)
{
    pthread_mutex_lock(&F.mu);
    F.steps[F.n++] = (step_t){net, status, body, delay_ms};
    pthread_mutex_unlock(&F.mu);
}

static int fake_nreq(void)
{
    pthread_mutex_lock(&F.mu);
    int n = F.nreq;
    pthread_mutex_unlock(&F.mu);
    return n;
}

static vc_net_t fake_post(void *ctx, const char *url, const char *key, const char *body, size_t body_len,
                          int timeout_ms, size_t max_resp, vc_http_resp_t *resp)
{
    (void)ctx;
    (void)max_resp;
    memset(resp, 0, sizeof(*resp));
    pthread_mutex_lock(&F.mu);
    int i = F.nreq;
    if (i < MAX_REQ) {
        F.url[i] = strdup(url);
        F.body[i] = strndup(body, body_len);
        F.key[i] = strdup(key);
        F.timeout[i] = timeout_ms;
        F.nreq++;
    }
    step_t s = F.pos < F.n ? F.steps[F.pos++] : (step_t){VC_NET_FAIL, 0, NULL, 0};
    if (F.cancel_on_req && F.cancel_at == i) {
        atomic_store(F.cancel_on_req, true);
    }
    if (F.virtual_time) {
        F.now += s.delay_ms;
    }
    bool vt = F.virtual_time;
    pthread_mutex_unlock(&F.mu);
    if (!vt && s.delay_ms) {
        usleep((useconds_t)s.delay_ms * 1000);
    }
    if (s.net != VC_NET_OK) {
        return s.net;
    }
    resp->status = s.status;
    if (s.body) {
        resp->body_len = strlen(s.body);
        resp->body = vc_big_malloc(resp->body_len + 1);
        memcpy(resp->body, s.body, resp->body_len + 1);
    }
    return VC_NET_OK;
}

static const vc_transport_t s_fake = {.post = fake_post, .ctx = NULL};

const vc_transport_t *vc_platform_transport(void)
{
    return &s_fake;
}

/* ---------------- 流水线测试（虚拟时钟） ---------------- */

typedef struct {
    voice_cloud_event_t evt;
    char text[512];
    char detail[256];
} ev_t;

typedef struct {
    ev_t ev[32];
    int n;
    atomic_bool cancel;
    int tool_calls;
    char tool_names[8][32];
    int tool_percent[8];
    esp_err_t tool_rc;
    const char *tool_out;
    int cancel_in_tool; /* 第几次工具调用时取消（-1 不取消） */
    int released;
} rec_t;

static void rec_emit(void *ctx, voice_cloud_event_t evt, const char *text, const char *detail)
{
    rec_t *r = ctx;
    if (r->n < 32) {
        r->ev[r->n].evt = evt;
        snprintf(r->ev[r->n].text, sizeof(r->ev[0].text), "%s", text);
        snprintf(r->ev[r->n].detail, sizeof(r->ev[0].detail), "%s", detail);
        r->n++;
    }
}

static bool rec_cancelled(void *ctx)
{
    return atomic_load(&((rec_t *)ctx)->cancel);
}

static int64_t vt_now(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&F.mu);
    int64_t n = F.now;
    pthread_mutex_unlock(&F.mu);
    return n;
}

static void vt_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    pthread_mutex_lock(&F.mu);
    F.now += ms;
    pthread_mutex_unlock(&F.mu);
}

static void rec_release(void *ctx)
{
    ((rec_t *)ctx)->released++;
}

static esp_err_t rec_tool(const char *name, const cJSON *args, char *out, size_t out_size, void *ctx)
{
    rec_t *r = ctx;
    if (r->cancel_in_tool == r->tool_calls) {
        atomic_store(&r->cancel, true);
    }
    if (r->tool_calls < 8) {
        snprintf(r->tool_names[r->tool_calls], 32, "%s", name);
        const cJSON *p = cJSON_GetObjectItem(args, "percent");
        r->tool_percent[r->tool_calls] = cJSON_IsNumber(p) ? p->valueint : -1;
    }
    r->tool_calls++;
    snprintf(out, out_size, "%s", r->tool_out ? r->tool_out : "");
    return r->tool_rc;
}

static char g_tools_json[4096];

static void job_init(vc_job_t *j, rec_t *r, const buf_t *frames, const char *text)
{
    memset(j, 0, sizeof(*j));
    memset(r, 0, sizeof(*r));
    atomic_init(&r->cancel, false);
    r->cancel_in_tool = -1;
    j->asr_url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    j->asr_key = "sk-asr-TEST";
    j->asr_model = "qwen3-asr-flash";
    j->chat_url = "https://api.deepseek.com/chat/completions";
    j->chat_key = "sk-chat-TEST";
    j->chat_model = "deepseek-flash";
    j->flavor = VC_FLAVOR_DEEPSEEK;
    j->asr_context = VC_ASR_CONTEXT;
    j->frames = frames ? frames->data : NULL;
    j->frames_len = frames ? frames->len : 0;
    j->text = text;
    char *n = vc_tools_normalize(voice_cloud_default_tools_json());
    snprintf(g_tools_json, sizeof(g_tools_json), "%s", n);
    free(n);
    j->tools_json = g_tools_json;
    j->tool_cb = rec_tool;
    j->tool_ctx = r;
    j->tp = &s_fake;
    j->emit = rec_emit;
    j->emit_ctx = r;
    j->cancelled = rec_cancelled;
    j->cancel_ctx = r;
    j->now_ms = vt_now;
    j->sleep_ms = vt_sleep;
    j->release_frames = rec_release;
    j->release_ctx = r;
    fake_reset();
    F.virtual_time = true;
}

#define ASR_OK "{\"choices\":[{\"message\":{\"content\":\"背夹，帮我把亮度调到50%。\"}}]}"
#define TOOL_BRIGHT                                                                                              \
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,\"tool_calls\":[{\"id\":\"call_1\","    \
    "\"type\":\"function\",\"function\":{\"name\":\"set_brightness\",\"arguments\":\"{\\\"percent\\\":50}\"}}]}," \
    "\"finish_reason\":\"tool_calls\"}]}"
#define REPLY_OK "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"好的，亮度已调到**50%**！😀\"}}]}"

static cJSON *req_json(int i)
{
    return cJSON_Parse(F.body[i]);
}

static void test_pipeline_voice_tools(void)
{
    buf_t f = fake_frames(30);
    vc_job_t j;
    rec_t r;
    job_init(&j, &r, &f, NULL);
    fake_add(VC_NET_OK, 200, ASR_OK, 900);
    fake_add(VC_NET_OK, 200, TOOL_BRIGHT, 1200);
    fake_add(VC_NET_OK, 200, REPLY_OK, 800);
    r.tool_out = "{\"brightness\":50}";
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(r.released == 1);
    CHECK(r.n == 4);
    CHECK(r.ev[0].evt == VOICE_CLOUD_EVT_RECOGNIZING);
    CHECK(r.ev[1].evt == VOICE_CLOUD_EVT_HEARD);
    CHECK_STR(r.ev[1].text, "背夹，帮我把亮度调到50%。");
    CHECK(r.ev[2].evt == VOICE_CLOUD_EVT_THINKING);
    CHECK(r.ev[3].evt == VOICE_CLOUD_EVT_TOOL);
    CHECK_STR(r.ev[3].text, "set_brightness");
    CHECK_STR(r.ev[3].detail, "{\"percent\":50}");
    CHECK(j.final.evt == VOICE_CLOUD_EVT_REPLY);
    CHECK_STR(j.final.text, "好的，亮度已调到50%！");
    CHECK_STR(j.out_user, "背夹，帮我把亮度调到50%。");
    CHECK_STR(j.out_reply, "好的，亮度已调到50%！");
    CHECK(r.tool_calls == 1 && r.tool_percent[0] == 50);
    CHECK(F.nreq == 3);
    CHECK_STR(F.url[0], j.asr_url);
    CHECK_STR(F.key[0], "sk-asr-TEST");
    CHECK(strstr(F.body[0], "\"input_audio\"") && strstr(F.body[0], "data:audio/ogg;base64,T2dnUw"));
    CHECK_STR(F.url[1], j.chat_url);
    CHECK_STR(F.key[1], "sk-chat-TEST");
    CHECK(strstr(F.body[1], "sk-") == NULL); /* 正文里不会出现密钥 */

    cJSON *r1 = req_json(1);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(r1, "messages")) == 2);
    CHECK_STR(cJSON_GetObjectItem(r1, "tool_choice")->valuestring, "auto");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetObjectItem(r1, "thinking"), "type")->valuestring, "disabled");
    cJSON_Delete(r1);
    cJSON *r2 = req_json(2);
    cJSON *m2 = cJSON_GetObjectItem(r2, "messages");
    CHECK(cJSON_GetArraySize(m2) == 4);
    cJSON *am = cJSON_GetArrayItem(m2, 2);
    CHECK_STR(cJSON_GetObjectItem(am, "role")->valuestring, "assistant");
    CHECK_STR(cJSON_GetObjectItem(am, "content")->valuestring, "");
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(am, "tool_calls"), 0), "id")->valuestring,
              "call_1");
    cJSON *tm = cJSON_GetArrayItem(m2, 3);
    CHECK_STR(cJSON_GetObjectItem(tm, "role")->valuestring, "tool");
    CHECK_STR(cJSON_GetObjectItem(tm, "tool_call_id")->valuestring, "call_1");
    cJSON *content = cJSON_Parse(cJSON_GetObjectItem(tm, "content")->valuestring);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(content, "ok")));
    CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(content, "result"), "brightness")->valueint == 50);
    cJSON_Delete(content);
    CHECK_STR(cJSON_GetObjectItem(r2, "tool_choice")->valuestring, "auto");
    cJSON_Delete(r2);
    vc_job_release(&j);
    free(f.data);
}

#define TOOL_VOL                                                                                                 \
    "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","        \
    "\"function\":{\"name\":\"set_volume\",\"arguments\":\"{\\\"percent\\\":30}\"}}]}}]}"
#define TOOL_MODE                                                                                                \
    "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":[{\"id\":\"c2\",\"type\":\"function\","        \
    "\"function\":{\"name\":\"set_mode\",\"arguments\":{\"mode\":\"dock\"}}}]}}]}"

static void test_pipeline_two_rounds(void)
{
    vc_job_t j;
    rec_t r;
    job_init(&j, &r, NULL, "音量调到三十，再切到底座模式");
    fake_add(VC_NET_OK, 200, TOOL_VOL, 0);
    fake_add(VC_NET_OK, 200, TOOL_MODE, 0);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"都办好了。\"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(r.tool_calls == 2);
    CHECK_STR(r.tool_names[0], "set_volume");
    CHECK_STR(r.tool_names[1], "set_mode");
    CHECK(r.n == 4 && r.ev[0].evt == VOICE_CLOUD_EVT_HEARD && r.ev[1].evt == VOICE_CLOUD_EVT_THINKING);
    CHECK(r.ev[2].evt == VOICE_CLOUD_EVT_TOOL && r.ev[3].evt == VOICE_CLOUD_EVT_TOOL);
    CHECK_STR(r.ev[3].detail, "{\"mode\":\"dock\"}"); /* arguments 直接给对象也能处理 */
    CHECK_STR(j.final.text, "都办好了。");
    CHECK(F.nreq == 3);
    cJSON *r3 = req_json(2);
    CHECK_STR(cJSON_GetObjectItem(r3, "tool_choice")->valuestring, "none"); /* 第 3 轮不再允许工具 */
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(r3, "messages")) == 6);
    cJSON_Delete(r3);
    vc_job_release(&j);

    /* 第 3 轮仍返回工具调用且没有文字：用“好的，已经办好了。”收尾，不执行第 3 轮的工具 */
    job_init(&j, &r, NULL, "测试");
    fake_add(VC_NET_OK, 200, TOOL_VOL, 0);
    fake_add(VC_NET_OK, 200, TOOL_MODE, 0);
    fake_add(VC_NET_OK, 200, TOOL_VOL, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(r.tool_calls == 2);
    CHECK_STR(j.final.text, VC_MSG_TOOLS_DONE);
    vc_job_release(&j);
}

static void test_pipeline_tool_errors(void)
{
    vc_job_t j;
    rec_t r;
    job_init(&j, &r, NULL, "测试");
    fake_add(VC_NET_OK, 200,
             "{\"choices\":[{\"message\":{\"content\":\"\",\"tool_calls\":["
             "{\"id\":\"a\",\"type\":\"function\",\"function\":{\"name\":\"format_disk\",\"arguments\":\"{}\"}},"
             "{\"id\":\"b\",\"type\":\"function\",\"function\":{\"name\":\"set_volume\",\"arguments\":\"{bad\"}},"
             "{\"id\":\"c\",\"type\":\"function\",\"function\":{\"name\":\"open_app\",\"arguments\":\"{\\\"name\\\":\\\"天气\\\"}\"}},"
             "{\"id\":\"d\",\"type\":\"function\",\"function\":{\"name\":\"set_volume\",\"arguments\":\"\"}},"
             "{\"id\":\"e\",\"type\":\"function\",\"function\":{\"name\":\"set_mode\",\"arguments\":\"{}\"}}]}}]}",
             0);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"抱歉，没有找到这个应用。\"}}]}", 0);
    r.tool_rc = ESP_ERR_NOT_SUPPORTED;
    r.tool_out = "不支持";
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(r.tool_calls == 2); /* 只执行了 open_app 与空参数的 set_volume（第 5 个超出每轮 4 个上限） */
    cJSON *r2 = req_json(1);
    cJSON *m = cJSON_GetObjectItem(r2, "messages");
    CHECK(cJSON_GetArraySize(m) == 2 + 1 + 5); /* 每个 tool_call 都有一条 tool 消息 */
    const char *expect[] = {"unknown_tool", "invalid_arguments", "ESP_ERR_NOT_SUPPORTED", "ESP_ERR_NOT_SUPPORTED",
                            "too_many_calls"};
    for (int i = 0; i < 5; i++) {
        cJSON *tm = cJSON_GetArrayItem(m, 3 + i);
        cJSON *c = cJSON_Parse(cJSON_GetObjectItem(tm, "content")->valuestring);
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(c, "ok")));
        CHECK_STR(cJSON_GetObjectItem(c, "error")->valuestring, expect[i]);
        if (i == 2) {
            CHECK_STR(cJSON_GetObjectItem(c, "message")->valuestring, "不支持");
        }
        cJSON_Delete(c);
    }
    cJSON_Delete(r2);
    vc_job_release(&j);
}

static void expect_error(vc_job_t *j, const char *text, const char *detail, int line)
{
    g_checks++;
    if (j->final.evt != VOICE_CLOUD_EVT_ERROR || !j->final.text || strcmp(j->final.text, text) != 0 ||
        strcmp(j->final.detail, detail) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL error (line %d): got %d \"%s\" / \"%s\", expect \"%s\" / \"%s\"\n", line, j->final.evt,
                j->final.text ? j->final.text : "(null)", j->final.detail, text, detail);
    }
}

static void test_pipeline_retry_and_errors(void)
{
    vc_job_t j;
    rec_t r;
    /* 503、503 后成功：两次退避 600 + 1200 ms */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_OK, 503, "Service is too busy", 100);
    fake_add(VC_NET_OK, 503, "Service is too busy", 100);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"你好呀\"}}]}", 100);
    int64_t t0 = F.now;
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(F.nreq == 3 && F.now - t0 == 300 + 600 + 1200);
    vc_job_release(&j);

    /* 连续 3 次 503：报错 */
    job_init(&j, &r, NULL, "你好");
    for (int i = 0; i < 3; i++) {
        fake_add(VC_NET_OK, 503, "{\"error\":{\"message\":\"busy\"}}", 0);
    }
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR);
    CHECK(F.nreq == 3);
    expect_error(&j, "服务暂时不可用，请稍后再试", "chat_http_503", __LINE__);
    vc_job_release(&j);

    /* 429 后成功 */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_OK, 429, "{}", 0);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK && F.nreq == 2);
    vc_job_release(&j);

    /* 401 不重试 */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_OK, 401, "{\"error\":{\"message\":\"Incorrect API key provided: sk-****\"}}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 1);
    expect_error(&j, "对话服务鉴权失败，请检查密钥", "chat_http_401", __LINE__);
    vc_job_release(&j);

    /* 400 但正文是 too busy：按繁忙重试 */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_OK, 400, "Service is too busy", 0);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK && F.nreq == 2);
    vc_job_release(&j);

    /* 网络错误 3 次 */
    job_init(&j, &r, NULL, "你好");
    for (int i = 0; i < 3; i++) {
        fake_add(VC_NET_FAIL, 0, NULL, 0);
    }
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 3);
    expect_error(&j, VC_MSG_NET, "chat_net", __LINE__);
    vc_job_release(&j);

    /* 响应过大 / 内存不足：不重试 */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_TOO_LARGE, 0, NULL, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 1);
    expect_error(&j, VC_MSG_BAD_RESPONSE, "chat_bad_response", __LINE__);
    vc_job_release(&j);
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_NOMEM, 0, NULL, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 1);
    expect_error(&j, VC_MSG_NO_MEM, "no_mem", __LINE__);
    vc_job_release(&j);

    /* 对话返回格式不对 / 内容为空 */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_OK, 200, "<html>", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR);
    expect_error(&j, VC_MSG_BAD_RESPONSE, "chat_bad_response", __LINE__);
    vc_job_release(&j);
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"😀\"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR);
    expect_error(&j, VC_MSG_CHAT_EMPTY, "chat_empty", __LINE__);
    vc_job_release(&j);

    /* ASR 各种失败 */
    buf_t f = fake_frames(10);
    job_init(&j, &r, &f, NULL);
    for (int i = 0; i < 3; i++) {
        fake_add(VC_NET_OK, 500, "{}", 0);
    }
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 3);
    expect_error(&j, "服务暂时不可用，请稍后再试", "asr_http_500", __LINE__);
    CHECK(r.n == 1 && r.ev[0].evt == VOICE_CLOUD_EVT_RECOGNIZING);
    vc_job_release(&j);
    job_init(&j, &r, &f, NULL);
    fake_add(VC_NET_OK, 200, "{\"choices\":5}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR);
    expect_error(&j, VC_MSG_BAD_RESPONSE, "asr_bad_response", __LINE__);
    vc_job_release(&j);
    job_init(&j, &r, &f, NULL);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\" \"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR);
    expect_error(&j, VC_MSG_ASR_EMPTY, "asr_empty", __LINE__);
    vc_job_release(&j);
    job_init(&j, &r, &f, NULL);
    fake_add(VC_NET_OK, 403, "{}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 1);
    expect_error(&j, "语音识别服务鉴权失败，请检查密钥", "asr_http_403", __LINE__);
    vc_job_release(&j);
    free(f.data);
}

static void test_pipeline_deadlines(void)
{
    vc_job_t j;
    rec_t r;
    /* 对话阶段 25 s：第 1 次超时用掉 15 s，退避 0.6 s，第 2 次只给剩下的 9.4 s，之后放弃 */
    job_init(&j, &r, NULL, "你好");
    fake_add(VC_NET_TIMEOUT, 0, NULL, 15000);
    fake_add(VC_NET_TIMEOUT, 0, NULL, 9400);
    fake_add(VC_NET_OK, 200, "{}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR);
    CHECK(F.nreq == 2 && F.timeout[0] == 15000 && F.timeout[1] == 9400);
    expect_error(&j, VC_MSG_TIMEOUT, "chat_timeout", __LINE__);
    vc_job_release(&j);

    /* ASR 阶段：一次 503 用掉 14 s，剩余时间不够退避再试，返回 503 而不是超时 */
    buf_t f = fake_frames(10);
    job_init(&j, &r, &f, NULL);
    fake_add(VC_NET_OK, 503, "{}", 14000);
    fake_add(VC_NET_OK, 200, ASR_OK, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_ERROR && F.nreq == 1);
    expect_error(&j, "服务暂时不可用，请稍后再试", "asr_http_503", __LINE__);
    vc_job_release(&j);
    free(f.data);
}

static void test_pipeline_cancel(void)
{
    vc_job_t j;
    rec_t r;
    buf_t f = fake_frames(10);
    /* ASR 请求进行中被取消：不再发出“听到”，也没有终态 */
    job_init(&j, &r, &f, NULL);
    F.cancel_on_req = &r.cancel;
    F.cancel_at = 0;
    fake_add(VC_NET_OK, 200, ASR_OK, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_CANCELLED);
    CHECK(r.n == 1 && j.final.text == NULL && j.out_user == NULL);
    vc_job_release(&j);
    /* 工具执行时被取消：后续不再请求 */
    job_init(&j, &r, &f, NULL);
    r.cancel_in_tool = 0;
    fake_add(VC_NET_OK, 200, ASR_OK, 0);
    fake_add(VC_NET_OK, 200, TOOL_BRIGHT, 0);
    fake_add(VC_NET_OK, 200, REPLY_OK, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_CANCELLED);
    CHECK(F.nreq == 2 && j.final.text == NULL);
    vc_job_release(&j);
    /* 退避期间取消 */
    job_init(&j, &r, NULL, "你好");
    F.cancel_on_req = &r.cancel;
    F.cancel_at = 1;
    fake_add(VC_NET_OK, 503, "{}", 0);
    fake_add(VC_NET_OK, 503, "{}", 0);
    fake_add(VC_NET_OK, 200, REPLY_OK, 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_CANCELLED && F.nreq == 2);
    vc_job_release(&j);
    free(f.data);
}

static void test_pipeline_history_and_no_tools(void)
{
    vc_job_t j;
    rec_t r;
    vc_turn_t hist[4] = {{"一", "1"}, {"二", "2"}, {"三", "3"}, {"四", "4"}};
    job_init(&j, &r, NULL, "五");
    j.history = hist;
    j.history_n = 4;
    j.tools_json = NULL; /* 没有工具：请求里不带 tools */
    j.now_text = "2026年10月16日 星期五 09:30";
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    cJSON *q = req_json(0);
    cJSON *m = cJSON_GetObjectItem(q, "messages");
    CHECK(cJSON_GetArraySize(m) == 1 + 8 + 1);
    CHECK(cJSON_GetObjectItem(q, "tools") == NULL);
    CHECK(strstr(cJSON_GetObjectItem(cJSON_GetArrayItem(m, 0), "content")->valuestring, "当前时间：2026年10月16日"));
    cJSON_Delete(q);
    vc_job_release(&j);
}

/* 本地指令：认得的句子不请求对话模型，回复直接用设备给的确认语 */
static int g_intent_calls;
static bool intent_volume(const char *text, char *reply, size_t size, void *ctx)
{
    (void)ctx;
    g_intent_calls++;
    if (strstr(text, "音量") && strstr(text, "40")) {
        snprintf(reply, size, "好的，音量已调到40%%。");
        return true;
    }
    return false;
}

static void test_pipeline_intent(void)
{
    buf_t f = fake_frames(30);
    vc_job_t j;
    rec_t r;
    job_init(&j, &r, &f, NULL);
    j.intent_cb = intent_volume;
    g_intent_calls = 0;
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"背夹，把音量调到40%。\"}}]}", 900);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(g_intent_calls == 1);
    CHECK(F.nreq == 1);                       /* 只有 ASR，没有对话请求 */
    CHECK(r.tool_calls == 0);
    CHECK(r.n == 2 && r.ev[1].evt == VOICE_CLOUD_EVT_HEARD);
    CHECK(j.final.evt == VOICE_CLOUD_EVT_REPLY);
    CHECK_STR(j.final.text, "好的，音量已调到40%。");
    CHECK_STR(j.out_user, "背夹，把音量调到40%。");
    CHECK_STR(j.out_reply, "好的，音量已调到40%。");
    vc_job_release(&j);
    free(f.data);

    /* 不认得的句子照常走对话模型 */
    job_init(&j, &r, NULL, "今天天气怎么样");
    j.intent_cb = intent_volume;
    g_intent_calls = 0;
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"多云，17度。\"}}]}", 0);
    CHECK(vc_pipeline_run(&j) == VC_RUN_OK);
    CHECK(g_intent_calls == 1);
    CHECK(F.nreq == 1);
    CHECK_STR(F.url[0], j.chat_url);
    CHECK_STR(j.final.text, "多云，17度。");
    vc_job_release(&j);
}

/* ---------------- 公开 API 测试（pthread 桩 + 假传输，真实线程） ---------------- */

static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    ev_t ev[64];
    int n;
    int terminal;
    bool busy_at_terminal;
    pthread_t thread_at_terminal;
} A = {.mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER};

static void api_reset_events(void)
{
    pthread_mutex_lock(&A.mu);
    A.n = 0;
    A.terminal = 0;
    pthread_mutex_unlock(&A.mu);
}

static void api_cb(voice_cloud_event_t evt, const char *text, const char *detail, void *ctx)
{
    (void)ctx;
    bool busy = false;
    if (evt == VOICE_CLOUD_EVT_REPLY || evt == VOICE_CLOUD_EVT_ERROR) {
        busy = voice_cloud_busy(); /* 终态回调时应已不忙 */
    }
    pthread_mutex_lock(&A.mu);
    if (A.n < 64) {
        A.ev[A.n].evt = evt;
        snprintf(A.ev[A.n].text, sizeof(A.ev[0].text), "%s", text ? text : "(null)");
        snprintf(A.ev[A.n].detail, sizeof(A.ev[0].detail), "%s", detail ? detail : "(null)");
        A.n++;
    }
    if (evt == VOICE_CLOUD_EVT_REPLY || evt == VOICE_CLOUD_EVT_ERROR) {
        A.terminal++;
        A.busy_at_terminal = busy;
        A.thread_at_terminal = pthread_self();
        pthread_cond_broadcast(&A.cv);
    }
    pthread_mutex_unlock(&A.mu);
}

static bool api_wait_terminal(int count, int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&A.mu);
    while (A.terminal < count) {
        if (pthread_cond_timedwait(&A.cv, &A.mu, &ts) != 0) {
            break;
        }
    }
    bool ok = A.terminal >= count;
    pthread_mutex_unlock(&A.mu);
    return ok;
}

static void wait_nreq(int n)
{
    for (int i = 0; i < 500 && fake_nreq() < n; i++) {
        usleep(2000);
    }
}

static void wait_idle(void)
{
    for (int i = 0; i < 500 && vc_internal_live_workers() > 0; i++) {
        usleep(10000);
    }
}

static struct {
    pthread_mutex_t mu;
    int calls;
    char last[32];
} T = {.mu = PTHREAD_MUTEX_INITIALIZER};

static esp_err_t api_tool(const char *name, const cJSON *args, char *out, size_t out_size, void *ctx)
{
    (void)args;
    (void)ctx;
    pthread_mutex_lock(&T.mu);
    T.calls++;
    snprintf(T.last, sizeof(T.last), "%s", name);
    pthread_mutex_unlock(&T.mu);
    snprintf(out, out_size, "已执行");
    return ESP_OK;
}

static void push_n(int n)
{
    uint8_t pkt[180];
    memset(pkt, 0x11, sizeof(pkt));
    pkt[0] = 0x48;
    for (int i = 0; i < n; i++) {
        CHECK(voice_cloud_push_opus(pkt, sizeof(pkt)) == ESP_OK);
    }
}

static void test_api_uninit_and_config(void)
{
    host_nvs_clear();
    vc_internal_reset();
    uint8_t pkt[4] = {0x48, 1, 2, 3};
    CHECK(voice_cloud_push_opus(pkt, 4) == ESP_ERR_INVALID_STATE);
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_ERR_INVALID_STATE);
    CHECK(voice_cloud_begin() == ESP_ERR_INVALID_STATE);
    CHECK(!voice_cloud_asr_ready() && !voice_cloud_chat_ready() && !voice_cloud_busy());
    cJSON *pub = voice_cloud_config_public();
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(pub, "asr_ready")));
    CHECK_STR(cJSON_GetObjectItem(pub, "asr_model")->valuestring, "qwen3-asr-flash");
    CHECK_STR(cJSON_GetObjectItem(pub, "chat_model")->valuestring, "deepseek-flash");
    cJSON_Delete(pub);

    CHECK(voice_cloud_init() == ESP_OK);
    CHECK(voice_cloud_init() == ESP_OK); /* 可重复调用 */
    CHECK(!voice_cloud_asr_ready() && !voice_cloud_chat_ready());

    /* 未配置：finish/ask_text 在调用方线程里同步回调 ERROR，返回 ESP_OK */
    api_reset_events();
    CHECK(voice_cloud_begin() == ESP_OK);
    push_n(10);
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_OK);
    CHECK(A.n == 1 && A.ev[0].evt == VOICE_CLOUD_EVT_ERROR);
    CHECK_STR(A.ev[0].text, "底座模式未配置 AI 服务");
    CHECK_STR(A.ev[0].detail, "not_configured");
    CHECK(pthread_equal(A.thread_at_terminal, pthread_self()));
    CHECK(voice_cloud_ask_text("你好", api_cb, NULL) == ESP_OK);
    CHECK(A.n == 2 && A.ev[1].evt == VOICE_CLOUD_EVT_ERROR);
    CHECK(voice_cloud_ask_text("", api_cb, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_ask_text("   ", api_cb, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_ask_text("\xff", api_cb, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_ask_text("你好", NULL, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_finish(NULL, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_ERR_INVALID_STATE); /* 没有 begin */

    /* 配置校验：类型、协议、主机、密钥字符 */
    CHECK(voice_cloud_config_set(NULL) == ESP_ERR_INVALID_ARG);
    cJSON *arr = cJSON_CreateArray();
    CHECK(voice_cloud_config_set(arr) == ESP_ERR_INVALID_ARG);
    cJSON_Delete(arr);
    const char *bad[] = {"{\"asr_key\":5}", "{\"asr_base\":\"http://dashscope.aliyuncs.com\"}",
                         "{\"chat_base\":\"https://u:p@x.com\"}", "{\"chat_key\":\"sk bad\"}",
                         "{\"chat_model\":\"a b\"}", "{\"asr_key\":\"sk-ok\",\"asr_base\":\"https://x.com?k=1\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cJSON *o = cJSON_Parse(bad[i]);
        CHECK(voice_cloud_config_set(o) == ESP_ERR_INVALID_ARG);
        cJSON_Delete(o);
    }
    CHECK(host_nvs_count("vcloud") == 0 && !voice_cloud_asr_ready()); /* 失败时什么都没写 */

    cJSON *ok = cJSON_Parse("{\"asr_base\":\" https://dashscope.aliyuncs.com/compatible-mode/v1/ \","
                            "\"asr_key\":\"sk-asr-TEST\",\"chat_key\":\"sk-chat-TEST\\n\",\"unknown\":1}");
    CHECK(voice_cloud_config_set(ok) == ESP_OK);
    cJSON_Delete(ok);
    CHECK(voice_cloud_asr_ready() && voice_cloud_chat_ready());
    CHECK_STR(host_nvs_peek("vcloud", "asr_base"), "https://dashscope.aliyuncs.com");
    CHECK_STR(host_nvs_peek("vcloud", "chat_key"), "sk-chat-TEST");
    pub = voice_cloud_config_public();
    char *printed = cJSON_PrintUnformatted(pub);
    CHECK(strstr(printed, "sk-") == NULL && strstr(printed, "TEST") == NULL); /* 绝不含密钥 */
    CHECK_STR(cJSON_GetObjectItem(pub, "asr_host")->valuestring, "dashscope.aliyuncs.com");
    CHECK_STR(cJSON_GetObjectItem(pub, "chat_host")->valuestring, "api.deepseek.com");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(pub, "asr_ready")) && cJSON_IsTrue(cJSON_GetObjectItem(pub, "chat_ready")));
    CHECK(cJSON_GetArraySize(pub) == 6);
    cJSON_free(printed);
    cJSON_Delete(pub);

    /* 持久化：重新初始化后仍然就绪 */
    vc_internal_reset();
    CHECK(!voice_cloud_asr_ready());
    CHECK(voice_cloud_init() == ESP_OK);
    CHECK(voice_cloud_asr_ready() && voice_cloud_chat_ready());

    /* 清除：""、null 都可；NVS 里的键被删掉 */
    cJSON *clr = cJSON_Parse("{\"chat_key\":\"\",\"asr_model\":null}");
    CHECK(voice_cloud_config_set(clr) == ESP_OK);
    cJSON_Delete(clr);
    CHECK(!voice_cloud_chat_ready() && host_nvs_peek("vcloud", "chat_key") == NULL);
    /* NVS 写失败：返回错误，内存里的配置不变 */
    host_nvs_fail_next_set(1);
    cJSON *again = cJSON_Parse("{\"chat_key\":\"sk-chat-TEST\"}");
    CHECK(voice_cloud_config_set(again) == ESP_FAIL);
    CHECK(!voice_cloud_chat_ready());
    CHECK(voice_cloud_config_set(again) == ESP_OK);
    CHECK(voice_cloud_chat_ready());
    cJSON_Delete(again);
    /* 换成百炼对话：chat_host 跟着变 */
    cJSON *ds = cJSON_Parse("{\"chat_base\":\"https://dashscope.aliyuncs.com\",\"chat_model\":\"qwen-plus\"}");
    CHECK(voice_cloud_config_set(ds) == ESP_OK);
    cJSON_Delete(ds);
    pub = voice_cloud_config_public();
    CHECK_STR(cJSON_GetObjectItem(pub, "chat_host")->valuestring, "dashscope.aliyuncs.com");
    CHECK_STR(cJSON_GetObjectItem(pub, "chat_model")->valuestring, "qwen-plus");
    cJSON_Delete(pub);
    cJSON *back = cJSON_Parse("{\"chat_base\":\"\",\"chat_model\":\"\"}");
    CHECK(voice_cloud_config_set(back) == ESP_OK);
    cJSON_Delete(back);
}

static void test_api_push_limits(void)
{
    uint8_t pkt[VC_OPUS_MAX_PACKET + 1];
    memset(pkt, 0x22, sizeof(pkt));
    voice_cloud_cancel();
    CHECK(voice_cloud_push_opus(pkt, 10) == ESP_ERR_INVALID_STATE);
    CHECK(voice_cloud_begin() == ESP_OK);
    CHECK(voice_cloud_push_opus(NULL, 10) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_push_opus(pkt, 0) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_push_opus(pkt, VC_OPUS_MAX_PACKET + 1) == ESP_ERR_INVALID_ARG);
    for (int i = 0; i < VC_MAX_PACKETS; i++) {
        if (voice_cloud_push_opus(pkt, 100) != ESP_OK) {
            CHECK(!"push failed before 2000 packets");
            break;
        }
    }
    CHECK(voice_cloud_push_opus(pkt, 100) == ESP_ERR_INVALID_SIZE); /* 超过 120 s 丢弃 */
    CHECK(voice_cloud_packet_count() == VC_MAX_PACKETS);
    /* 字节上限：每包 1275 字节时 512 KB 能放 410 包 */
    CHECK(voice_cloud_begin() == ESP_OK);
    CHECK(voice_cloud_packet_count() == 0);
    int stored = 0;
    while (voice_cloud_push_opus(pkt, VC_OPUS_MAX_PACKET) == ESP_OK) {
        stored++;
    }
    CHECK(stored == (int)(VC_STORE_CAP / (VC_OPUS_MAX_PACKET + 2)));
    voice_cloud_cancel();
    CHECK(voice_cloud_packet_count() == 0);
}

static void test_api_voice_session(void)
{
    CHECK(voice_cloud_set_tools("{bad", api_tool, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_set_tools(voice_cloud_default_tools_json(), NULL, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(voice_cloud_set_tools(voice_cloud_default_tools_json(), api_tool, NULL) == ESP_OK);
    fake_reset();
    F.virtual_time = false;
    fake_add(VC_NET_OK, 200, ASR_OK, 50);
    fake_add(VC_NET_OK, 200, TOOL_BRIGHT, 50);
    fake_add(VC_NET_OK, 200, REPLY_OK, 50);
    api_reset_events();
    CHECK(voice_cloud_begin() == ESP_OK);
    push_n(50);
    CHECK(voice_cloud_packet_count() == 50);
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_OK);
    CHECK(voice_cloud_busy());
    CHECK(voice_cloud_packet_count() == 50);
    CHECK(voice_cloud_push_opus((const uint8_t *)"\x48", 1) == ESP_ERR_INVALID_STATE); /* 已交给工作任务 */
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    pthread_mutex_lock(&A.mu);
    CHECK(A.n == 5);
    voice_cloud_event_t order[] = {VOICE_CLOUD_EVT_RECOGNIZING, VOICE_CLOUD_EVT_HEARD, VOICE_CLOUD_EVT_THINKING,
                                   VOICE_CLOUD_EVT_TOOL, VOICE_CLOUD_EVT_REPLY};
    for (int i = 0; i < 5 && i < A.n; i++) {
        CHECK(A.ev[i].evt == order[i]);
    }
    CHECK_STR(A.ev[1].text, "背夹，帮我把亮度调到50%。");
    CHECK_STR(A.ev[3].text, "set_brightness");
    CHECK_STR(A.ev[4].text, "好的，亮度已调到50%！");
    CHECK_STR(A.ev[0].text, ""); /* 没有内容时给空串而不是 NULL */
    CHECK(!A.busy_at_terminal);
    CHECK(!pthread_equal(A.thread_at_terminal, pthread_self()));
    pthread_mutex_unlock(&A.mu);
    CHECK(T.calls == 1);
    CHECK(!voice_cloud_busy() && vc_internal_live_workers() == 0);
    /* 请求：ASR 用 ASR 密钥、对话用对话密钥，地址来自配置 */
    CHECK_STR(F.url[0], "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions");
    CHECK_STR(F.key[0], "sk-asr-TEST");
    CHECK_STR(F.url[1], "https://api.deepseek.com/chat/completions");
    CHECK_STR(F.key[1], "sk-chat-TEST");
    CHECK(strstr(F.body[1], "\"thinking\":{\"type\":\"disabled\"}") != NULL);
}

static void test_api_busy_cancel_history(void)
{
    /* 忙：第二个 ask_text / finish 被拒，录音保留 */
    fake_reset();
    F.virtual_time = false;
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"答一\"}}]}", 300);
    api_reset_events();
    voice_cloud_history_clear();
    CHECK(voice_cloud_ask_text("问一", api_cb, NULL) == ESP_OK);
    CHECK(voice_cloud_busy());
    wait_nreq(1); /* 等“问一”的请求真正发出，避免与后面的新会话抢假传输的应答顺序 */
    CHECK(voice_cloud_ask_text("问二", api_cb, NULL) == ESP_ERR_INVALID_STATE);
    CHECK(voice_cloud_begin() == ESP_OK); /* begin 会取消正在进行的“问一” */
    CHECK(!voice_cloud_busy());
    push_n(5);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"问三\"}}]}", 0);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"答三\"}}]}", 0);
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_OK); /* 允许一个收尾中的会话 + 一个新会话 */
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    usleep(400000);
    pthread_mutex_lock(&A.mu);
    CHECK(A.terminal == 1); /* 被取消的“问一”不再回调 */
    CHECK_STR(A.ev[A.n - 1].text, "答三");
    pthread_mutex_unlock(&A.mu);

    /* finish 在忙时返回 INVALID_STATE 且录音保留（仍可继续收包），空闲后再 finish 即可 */
    fake_reset();
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"答四\"}}]}", 300);
    api_reset_events();
    CHECK(voice_cloud_begin() == ESP_OK);
    push_n(5);
    CHECK(voice_cloud_ask_text("问四", api_cb, NULL) == ESP_OK); /* ask_text 不动录音 */
    CHECK(voice_cloud_busy());
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_ERR_INVALID_STATE);
    CHECK(voice_cloud_packet_count() == 5);
    push_n(1);
    CHECK(voice_cloud_packet_count() == 6);
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"问六\"}}]}", 0);
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"答六\"}}]}", 0);
    api_reset_events();
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_OK);
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    CHECK(strstr(F.body[1], "\"data:audio/ogg;base64,") != NULL);
    CHECK(A.n == 4 && strcmp(A.ev[1].text, "问六") == 0 && strcmp(A.ev[3].text, "答六") == 0);

    /* cancel：没有任何回调，忙状态立即解除，之后可以再问 */
    fake_reset();
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"不该出现\"}}]}", 300);
    api_reset_events();
    CHECK(voice_cloud_ask_text("问五", api_cb, NULL) == ESP_OK);
    usleep(50000);
    voice_cloud_cancel();
    CHECK(!voice_cloud_busy());
    CHECK(!api_wait_terminal(1, 700));
    wait_idle();
    CHECK(vc_internal_live_workers() == 0);

    /* 历史：此前成功的是“问三/答三”“问四/答四”“问六/答六”，再问 3 次后只保留最近 4 轮 */
    for (int i = 0; i < 3; i++) {
        fake_reset();
        fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
        api_reset_events();
        CHECK(voice_cloud_ask_text("再问", api_cb, NULL) == ESP_OK);
        CHECK(api_wait_terminal(1, 5000));
        wait_idle();
    }
    fake_reset();
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
    api_reset_events();
    CHECK(voice_cloud_ask_text("最后", api_cb, NULL) == ESP_OK);
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    cJSON *q = cJSON_Parse(F.body[0]);
    cJSON *m = cJSON_GetObjectItem(q, "messages");
    CHECK(cJSON_GetArraySize(m) == 1 + 8 + 1);
    CHECK_STR(cJSON_GetObjectItem(cJSON_GetArrayItem(m, 1), "content")->valuestring, "问六"); /* 问三、问四已被挤出 */
    cJSON_Delete(q);
    voice_cloud_history_clear();
    fake_reset();
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
    api_reset_events();
    CHECK(voice_cloud_ask_text("清空后", api_cb, NULL) == ESP_OK);
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    q = cJSON_Parse(F.body[0]);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(q, "messages")) == 2);
    CHECK(strstr(F.body[0], "\"tools\"") != NULL);
    cJSON_Delete(q);

    /* 清掉工具：请求里不再带 tools */
    CHECK(voice_cloud_set_tools(NULL, NULL, NULL) == ESP_OK);
    fake_reset();
    fake_add(VC_NET_OK, 200, "{\"choices\":[{\"message\":{\"content\":\"好\"}}]}", 0);
    api_reset_events();
    CHECK(voice_cloud_ask_text("无工具", api_cb, NULL) == ESP_OK);
    CHECK(api_wait_terminal(1, 5000));
    wait_idle();
    CHECK(strstr(F.body[0], "\"tools\"") == NULL);

    /* 建任务失败：返回 ESP_ERR_NO_MEM，没有回调，也不残留忙状态 */
    host_task_fail_next(2);
    api_reset_events();
    CHECK(voice_cloud_ask_text("失败", api_cb, NULL) == ESP_ERR_NO_MEM);
    CHECK(!voice_cloud_busy() && vc_internal_live_workers() == 0 && A.n == 0);

    /* 录音太短 */
    CHECK(voice_cloud_begin() == ESP_OK);
    push_n(2);
    api_reset_events();
    CHECK(voice_cloud_finish(api_cb, NULL) == ESP_OK);
    CHECK(A.n == 1 && strcmp(A.ev[0].detail, "no_audio") == 0);
    vc_internal_reset();
    host_nvs_clear();
    fake_reset();
}

/* ---------------- 命令行工具模式 ---------------- */

static int cli(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "--mux") == 0) {
        buf_t f = read_file(argv[2]);
        vc_ogg_opts_t o;
        vc_ogg_opts_default(&o);
        if (argc >= 5) {
            o.vendor = argv[4];
        }
        buf_t ogg = {0};
        int r = vc_ogg_mux(f.data, f.len, &o, buf_sink, &ogg);
        if (r == 0) {
            r = write_file(argv[3], ogg.data, ogg.len);
        }
        printf("{\"ok\":%s,\"bytes\":%zu,\"size_fn\":%zu}\n", r == 0 ? "true" : "false", ogg.len,
               vc_ogg_size(f.data, f.len, &o));
        free(f.data);
        free(ogg.data);
        return r == 0 ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "--crc-check") == 0) {
        buf_t d = read_file(argv[2]);
        size_t pos = 0;
        int pages = 0, bad = 0;
        while (pos + 27 <= d.len && memcmp(d.data + pos, "OggS", 4) == 0) {
            int nseg = d.data[pos + 26];
            size_t end = pos + 27 + (size_t)nseg;
            for (int i = 0; i < nseg; i++) {
                end += d.data[pos + 27 + (size_t)i];
            }
            if (end > d.len) {
                bad++;
                break;
            }
            uint32_t stored = le32(d.data + pos + 22);
            uint8_t zero[4] = {0, 0, 0, 0};
            uint32_t c = vc_ogg_crc(0, d.data + pos, 22);
            c = vc_ogg_crc(c, zero, 4);
            c = vc_ogg_crc(c, d.data + pos + 26, end - pos - 26);
            if (c != stored) {
                bad++;
            }
            pages++;
            pos = end;
        }
        printf("{\"pages\":%d,\"bad\":%d,\"consumed\":%s}\n", pages, bad, pos == d.len ? "true" : "false");
        free(d.data);
        return (bad == 0 && pages > 0 && pos == d.len) ? 0 : 1;
    }
    if (argc >= 4 && strcmp(argv[1], "--asr-body") == 0) {
        buf_t f = read_file(argv[2]);
        size_t len = 0, ogg = 0;
        char *body = vc_asr_build_body(f.data, f.len, VC_DEFAULT_ASR_MODEL, VC_ASR_CONTEXT, &len, &ogg);
        int r = body ? write_file(argv[3], body, len) : -1;
        printf("{\"ok\":%s,\"bytes\":%zu,\"ogg\":%zu,\"packets\":%ld}\n", r == 0 ? "true" : "false", len, ogg,
               vc_frames_count(f.data, f.len));
        vc_big_free(body);
        free(f.data);
        return r == 0 ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "--user-strings") == 0) {
        const char *s[] = {VC_MSG_NOT_CONFIGURED, VC_MSG_NO_AUDIO, VC_MSG_NO_MEM, VC_MSG_NET, VC_MSG_TIMEOUT,
                           VC_MSG_BAD_RESPONSE, VC_MSG_ASR_EMPTY, VC_MSG_CHAT_EMPTY, VC_MSG_TOOLS_DONE};
        for (size_t i = 0; i < sizeof(s) / sizeof(s[0]); i++) {
            printf("%s\n", s[i]);
        }
        int codes[] = {400, 401, 402, 403, 404, 418, 429, 500, 503};
        for (int st = 0; st < 2; st++) {
            for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
                char d[40];
                printf("%s\n", vc_err_http((vc_stage_t)st, codes[i], d, sizeof(d)));
            }
        }
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "--clean") == 0) {
        size_t max = argc >= 4 ? (size_t)atoi(argv[3]) : VC_REPLY_MAX_BYTES;
        char *out = malloc(max + 1);
        vc_text_clean(argv[2], out, max + 1, max);
        printf("%s\n", out);
        free(out);
        return 0;
    }
    fprintf(stderr, "unknown arguments\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        return cli(argc, argv);
    }
    test_base64();
    test_frames_iter();
    test_ogg_structure();
    test_ogg_lacing();
    test_crc_table();
    test_asr_body();
    test_asr_parse();
    test_urls();
    test_chat_body();
    test_tools_normalize();
    test_text_clean();
    test_errors();
    test_pipeline_voice_tools();
    test_pipeline_two_rounds();
    test_pipeline_tool_errors();
    test_pipeline_retry_and_errors();
    test_pipeline_deadlines();
    test_pipeline_cancel();
    test_pipeline_history_and_no_tools();
    test_pipeline_intent();
    test_api_uninit_and_config();
    test_api_push_limits();
    test_api_voice_session();
    test_api_busy_cancel_history();
    fake_reset();
    printf("%s: %d checks, %d failures\n", g_failures ? "FAILED" : "PASSED", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
