/*
 * 回复文字清洗：让模型输出适合背夹屏幕显示。
 *
 * 设备默认字体 fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf 共 3680 个字形（已用 fontTools
 * 逐一核对 cmap 与轮廓）：ASCII 只缺 '!' 和 '^'；常用中文标点（，。！？：；、（）《》【】「」『』
 * “”‘’…—·～￥）都有；° → ℃ ％ ＋ × ÷ 及各种表情、方块、箭头都没有。
 * 所以这里：去掉 Markdown 记号与表情；'!' 换成全角“！”、丢弃 '^'；° ℃ → “度”，→ → “->”，
 * 不在字库里的全角 ASCII 换回半角；其余字库外的常见符号去掉；最后合并空白、按 UTF-8 截断。
 * 纯 C，不依赖 ESP-IDF。
 */
#include <stdio.h>
#include <string.h>

#include "vc_internal.h"

#define VC_CLEAN_INPUT_MAX 8192 /* 只处理前 8 KB，回复本来就很短 */

/* ---------------- UTF-8 基础 ---------------- */

/* 解码一个码点；返回消耗的字节数，非法序列返回 0 */
static size_t utf8_decode(const unsigned char *s, size_t len, uint32_t *cp)
{
    if (len == 0) {
        return 0;
    }
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t n;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) {
        n = 2;
        v = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        n = 3;
        v = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        n = 4;
        v = c & 0x07;
        min = 0x10000;
    } else {
        return 0;
    }
    if (len < n) {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) {
        return 0; /* 过长编码、超范围或代理区 */
    }
    *cp = v;
    return n;
}

bool vc_utf8_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    uint32_t cp;
    while (i < len) {
        size_t n = utf8_decode(p + i, len - i, &cp);
        if (!n) {
            return false;
        }
        i += n;
    }
    return true;
}

size_t vc_utf8_cut(const char *s, size_t len, size_t max)
{
    if (len <= max) {
        return len;
    }
    size_t cut = max;
    /* 切点落在后续字节上就往前退到字符起点，整个字符不要 */
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) {
        cut--;
    }
    return cut;
}

/* ---------------- 输出缓冲 ---------------- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool oom;
} vc_sb_t;

static void sb_put(vc_sb_t *b, const char *s, size_t n)
{
    if (b->oom || n == 0) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = (b->len + n + 1) * 2;
        char *nb = realloc(b->buf, cap);
        if (!nb) {
            b->oom = true;
            return;
        }
        b->buf = nb;
        b->cap = cap;
    }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}

static void sb_puts(vc_sb_t *b, const char *s)
{
    sb_put(b, s, strlen(s));
}

static void sb_putcp(vc_sb_t *b, uint32_t cp)
{
    char t[4];
    size_t n;
    if (cp < 0x80) {
        t[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        t[0] = (char)(0xC0 | (cp >> 6));
        t[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        t[0] = (char)(0xE0 | (cp >> 12));
        t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        t[0] = (char)(0xF0 | (cp >> 18));
        t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        t[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    sb_put(b, t, n);
}

/* 追加空格：开头或已有空格时不加（合并连续空白） */
static void sb_space(vc_sb_t *b)
{
    if (b->len && b->buf[b->len - 1] != ' ') {
        sb_put(b, " ", 1);
    }
}

/* 末尾码点（缓冲为空返回 0） */
static uint32_t sb_last_cp(const vc_sb_t *b)
{
    if (!b->len) {
        return 0;
    }
    size_t i = b->len - 1;
    while (i > 0 && ((unsigned char)b->buf[i] & 0xC0) == 0x80) {
        i--;
    }
    uint32_t cp = 0;
    if (!utf8_decode((const unsigned char *)b->buf + i, b->len - i, &cp)) {
        return 0;
    }
    return cp;
}

static void sb_rtrim(vc_sb_t *b)
{
    while (b->len && b->buf[b->len - 1] == ' ') {
        b->buf[--b->len] = '\0';
    }
}

/* ---------------- 符号映射 ---------------- */

/* 设备字库里有的全角 ASCII（U+FF01–U+FF5E 中的这些保留，其余换回半角） */
static bool fullwidth_in_font(uint32_t cp)
{
    switch (cp) {
    case 0xFF01: case 0xFF02: case 0xFF07: case 0xFF08: case 0xFF09: case 0xFF0C:
    case 0xFF1A: case 0xFF1B: case 0xFF1F: case 0xFF3B: case 0xFF3D: case 0xFF3F:
    case 0xFF40: case 0xFF5C: case 0xFF5E:
        return true;
    default:
        return false;
    }
}

/* 中文句读：换行拼接时这些字符后面不再补逗号 */
static bool is_cjk_punct(uint32_t cp)
{
    switch (cp) {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF01: case 0xFF1F: case 0xFF1A: case 0xFF1B:
    case 0x2026: case 0x2014: case 0xFF09: case 0x300D: case 0x300F: case 0x3011: case 0x300B:
    case 0x3009: case 0x201C: case 0x201D: case 0x2018: case 0x2019: case 0xFF5E:
        return true;
    default:
        return false;
    }
}

/* 截断时可作为句末的字符（不含半角 '.'，避免切在“1.5”这类数字中间） */
static bool is_sentence_end(uint32_t cp)
{
    return cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F || cp == 0xFF1B || cp == '?';
}

/* 必须丢弃的码点：表情、杂项符号、方块、制表符、零宽字符、私用区、辅助平面等 */
static bool drop_cp(uint32_t cp)
{
    if (cp >= 0x10000) {
        return true; /* 表情与扩展汉字，字库都没有 */
    }
    if (cp >= 0x80 && cp <= 0x9F) {
        return true;
    }
    if (cp == 0x00A9 || cp == 0x00AE || cp == 0x2122 || cp == 0x2139 || cp == 0x24C2 || cp == 0xFEFF ||
        cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299) {
        return true;
    }
    if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x2300 && cp <= 0x23FF) ||
        (cp >= 0x2460 && cp <= 0x24FF) || (cp >= 0x2500 && cp <= 0x27BF) || (cp >= 0x2900 && cp <= 0x297F) ||
        (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xFE00 && cp <= 0xFE1F) ||
        (cp >= 0xFFF0)) {
        return true;
    }
    return false;
}

/*
 * 写出一个码点（已做映射）。next 是后一个码点（用于 °C 这类组合），返回需要额外跳过的码点数。
 */
static int put_mapped(vc_sb_t *b, uint32_t cp, uint32_t next)
{
    if (cp < 0x80) {
        switch (cp) {
        case '!':
            sb_putcp(b, 0xFF01); /* 字库缺半角 '!' */
            return 0;
        case '^':
            return 0; /* 字库缺 '^'，直接去掉 */
        case '\t':
            sb_space(b);
            return 0;
        case ' ':
            sb_space(b);
            return 0;
        default:
            if (cp < 0x20 || cp == 0x7F) {
                return 0; /* 其他控制字符 */
            }
            sb_putcp(b, cp);
            return 0;
        }
    }
    switch (cp) {
    case 0x00A0: case 0x3000: case 0x2002: case 0x2003: case 0x2009: /* 各种空格 */
        sb_space(b);
        return 0;
    case 0x00B0: /* ° */
        if (next == 'C' || next == 'c') {
            sb_puts(b, "度");
            return 1;
        }
        if (next == 'F' || next == 'f') {
            sb_puts(b, "华氏度");
            return 1;
        }
        sb_puts(b, "度");
        return 0;
    case 0x2103: /* ℃ */
        sb_puts(b, "度");
        return 0;
    case 0x2109: /* ℉ */
        sb_puts(b, "华氏度");
        return 0;
    case 0x00D7: /* × */
        sb_puts(b, "x");
        return 0;
    case 0x00F7: /* ÷ */
        sb_puts(b, "/");
        return 0;
    case 0x00B1: /* ± */
        sb_puts(b, "+-");
        return 0;
    case 0x2212: case 0x2013: /* − – */
        sb_puts(b, "-");
        return 0;
    case 0x2264: /* ≤ */
        sb_puts(b, "<=");
        return 0;
    case 0x2265: /* ≥ */
        sb_puts(b, ">=");
        return 0;
    case 0x2260: /* ≠ */
        sb_puts(b, "不等于");
        return 0;
    case 0x2248: /* ≈ */
        sb_puts(b, "约");
        return 0;
    case 0x00A5: /* ¥ → 字库里有的全角 ￥ */
        sb_putcp(b, 0xFFE5);
        return 0;
    case 0x20AC: /* € */
        sb_puts(b, "欧元");
        return 0;
    case 0x00A3: /* £ */
        sb_puts(b, "英镑");
        return 0;
    case 0x2192: case 0x21D2: case 0x27A1: case 0x2794: case 0x279C: case 0x27F6: /* → ⇒ ➡ */
        sb_puts(b, "->");
        return 0;
    case 0x2190: case 0x21D0: case 0x2B05: case 0x27F5: /* ← ⇐ ⬅ */
        sb_puts(b, "<-");
        return 0;
    case 0x2022: case 0x25CF: case 0x25E6: case 0x2043: /* • ● ◦ ⁃ 当作分隔 */
        sb_space(b);
        return 0;
    default:
        break;
    }
    if (cp >= 0x2460 && cp <= 0x2473) { /* ①–⑳ → “1.” */
        char t[8];
        int n = snprintf(t, sizeof(t), "%u.", (unsigned)(cp - 0x2460 + 1));
        sb_put(b, t, (size_t)n);
        return 0;
    }
    if (cp >= 0xFF01 && cp <= 0xFF5E && !fullwidth_in_font(cp)) {
        uint32_t ascii = cp - 0xFEE0; /* 全角换回半角 */
        if (ascii != '^') {
            sb_putcp(b, ascii);
        }
        return 0;
    }
    if (drop_cp(cp)) {
        return 0;
    }
    sb_putcp(b, cp);
    return 0;
}

/* ---------------- Markdown ---------------- */

static bool is_space_byte(char c)
{
    return c == ' ' || c == '\t';
}

/* 去掉行首的标题、引用、列表、勾选框记号；返回内容起点 */
static size_t strip_line_prefix(const char *s, size_t len)
{
    size_t i = 0;
    bool changed = true;
    while (changed) {
        changed = false;
        while (i < len && is_space_byte(s[i])) {
            i++;
        }
        /* 标题：#… 后跟空格或行尾 */
        size_t h = i;
        while (h < len && s[h] == '#') {
            h++;
        }
        if (h > i && (h == len || is_space_byte(s[h]))) {
            i = h;
            changed = true;
            continue;
        }
        /* 引用 */
        if (i < len && s[i] == '>') {
            i++;
            changed = true;
            continue;
        }
        /* 无序列表：- * + 后跟空格；• 后跟空格 */
        if (i + 1 < len && (s[i] == '-' || s[i] == '*' || s[i] == '+') && is_space_byte(s[i + 1])) {
            i += 2;
            changed = true;
            continue;
        }
        if (i + 3 < len && (unsigned char)s[i] == 0xE2 && (unsigned char)s[i + 1] == 0x80 &&
            (unsigned char)s[i + 2] == 0xA2 && is_space_byte(s[i + 3])) {
            i += 4;
            changed = true;
            continue;
        }
        /* 勾选框 [ ] / [x] */
        if (i + 3 < len && s[i] == '[' && (s[i + 1] == ' ' || s[i + 1] == 'x' || s[i + 1] == 'X') &&
            s[i + 2] == ']' && is_space_byte(s[i + 3])) {
            i += 4;
            changed = true;
            continue;
        }
        /* 有序列表：1. / 1) 后跟空格，或 1、 */
        size_t d = i;
        while (d < len && d - i < 3 && s[d] >= '0' && s[d] <= '9') {
            d++;
        }
        if (d > i && d < len) {
            if ((s[d] == '.' || s[d] == ')') && (d + 1 == len || is_space_byte(s[d + 1]))) {
                i = d + 1;
                changed = true;
                continue;
            }
            if (d + 2 < len && (unsigned char)s[d] == 0xE3 && (unsigned char)s[d + 1] == 0x80 &&
                (unsigned char)s[d + 2] == 0x81) { /* “、” */
                i = d + 3;
                changed = true;
                continue;
            }
        }
    }
    return i;
}

/* 整行都是分隔线（--- *** ___）或表格分隔行（|---|:--|）时丢弃 */
static bool is_rule_line(const char *s, size_t len)
{
    size_t marks = 0, pipes = 0, dashes = 0;
    char mark = 0;
    bool only_rule = true, only_table = true;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (is_space_byte(c)) {
            continue;
        }
        if (c == '-' || c == '*' || c == '_') {
            if (!mark) {
                mark = c;
            }
            if (c != mark) {
                only_rule = false;
            }
            marks++;
        } else {
            only_rule = false;
        }
        if (c == '|') {
            pipes++;
        } else if (c == '-') {
            dashes++;
        } else if (c != ':') {
            only_table = false;
        }
    }
    if (only_rule && marks >= 3) {
        return true;
    }
    return only_table && pipes > 0 && dashes > 0;
}

/* 处理一行的行内记号并写入 line */
static void clean_inline(const char *s, size_t len, vc_sb_t *line)
{
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    size_t link_close = (size_t)-1, link_end = 0; /* 处于 [文字](地址) 中时记录位置 */
    while (i < len) {
        if (i == link_close) {
            i = link_end; /* 跳过 ](地址) */
            link_close = (size_t)-1;
            continue;
        }
        char c = s[i];
        if ((c == '[' || (c == '!' && i + 1 < len && s[i + 1] == '[')) && link_close == (size_t)-1) {
            size_t open = c == '[' ? i : i + 1;
            const char *close = memchr(s + open, ']', len - open);
            if (close && (size_t)(close - s) + 1 < len && close[1] == '(') {
                const char *end = memchr(close + 2, ')', len - (size_t)(close + 2 - s));
                if (end) {
                    link_close = (size_t)(close - s);
                    link_end = (size_t)(end - s) + 1;
                    i = open + 1;
                    continue;
                }
            }
        }
        if (c == '*' || c == '`') {
            i++;
            continue;
        }
        if ((c == '_' || c == '~') && i + 1 < len && s[i + 1] == c) {
            i += 2; /* __粗体__ 与 ~~删除线~~ */
            continue;
        }
        if (c == '|') {
            sb_space(line);
            i++;
            continue;
        }
        uint32_t cp;
        size_t n = utf8_decode(u + i, len - i, &cp);
        if (!n) {
            i++; /* 非法字节直接丢 */
            continue;
        }
        uint32_t next = 0;
        if (i + n < len) {
            uint32_t nx;
            if (utf8_decode(u + i + n, len - i - n, &nx)) {
                next = nx;
            }
        }
        int skip = put_mapped(line, cp, next);
        i += n;
        while (skip-- > 0 && i < len) {
            uint32_t tmp;
            size_t m = utf8_decode(u + i, len - i, &tmp);
            i += m ? m : 1;
        }
    }
}

/* 把一行接到输出后面：中文后补“，”，英文后补空格，已有标点则不补 */
static void join_line(vc_sb_t *out, const vc_sb_t *line)
{
    if (!line->len) {
        return;
    }
    if (out->len) {
        uint32_t last = sb_last_cp(out);
        if (last < 0x80) {
            if (last != ' ') {
                sb_put(out, " ", 1);
            }
        } else if (!is_cjk_punct(last)) {
            sb_puts(out, "，");
        }
    }
    const char *p = line->buf;
    size_t n = line->len;
    if (out->len && out->buf[out->len - 1] == ' ' && n && p[0] == ' ') {
        p++;
        n--;
    }
    sb_put(out, p, n);
}

size_t vc_text_clean(const char *in, char *out, size_t out_size, size_t max_bytes)
{
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!in) {
        return 0;
    }
    if (max_bytes + 1 > out_size) {
        max_bytes = out_size - 1;
    }
    size_t in_len = strlen(in);
    if (in_len > VC_CLEAN_INPUT_MAX) {
        in_len = vc_utf8_cut(in, in_len, VC_CLEAN_INPUT_MAX);
    }

    vc_sb_t acc = {0};
    vc_sb_t line = {0};
    bool in_fence = false;
    size_t pos = 0;
    while (pos <= in_len) {
        const char *nl = memchr(in + pos, '\n', in_len - pos);
        size_t end = nl ? (size_t)(nl - in) : in_len;
        size_t ll = end - pos;
        const char *ls = in + pos;
        if (ll && ls[ll - 1] == '\r') {
            ll--;
        }
        /* 代码块：``` 或 ~~~ 围起来的整段丢掉 */
        size_t k = 0;
        while (k < ll && is_space_byte(ls[k])) {
            k++;
        }
        if (ll - k >= 3 && (memcmp(ls + k, "```", 3) == 0 || memcmp(ls + k, "~~~", 3) == 0)) {
            in_fence = !in_fence;
        } else if (!in_fence && !is_rule_line(ls, ll)) {
            size_t start = strip_line_prefix(ls, ll);
            line.len = 0;
            if (line.buf) {
                line.buf[0] = '\0';
            }
            clean_inline(ls + start, ll - start, &line);
            sb_rtrim(&line);
            join_line(&acc, &line);
        }
        if (!nl) {
            break;
        }
        pos = end + 1;
    }
    free(line.buf);
    if (acc.oom || !acc.buf) {
        free(acc.buf);
        return 0;
    }
    sb_rtrim(&acc);
    const char *s = acc.buf;
    size_t len = acc.len;
    while (len && *s == ' ') {
        s++;
        len--;
    }

    size_t n;
    if (len <= max_bytes) {
        n = len;
        memcpy(out, s, n);
    } else {
        /* 优先在句末截断（不加省略号）；句末太靠前时硬截并补“…” */
        const unsigned char *u = (const unsigned char *)s;
        size_t i = 0, best = 0;
        uint32_t cp;
        while (i < len) {
            size_t m = utf8_decode(u + i, len - i, &cp);
            if (!m || i + m > max_bytes) {
                break;
            }
            i += m;
            if (is_sentence_end(cp)) {
                best = i;
            }
        }
        if (best && best * 10 >= max_bytes * 6) {
            n = best;
            memcpy(out, s, n);
        } else if (max_bytes >= 3) {
            n = vc_utf8_cut(s, len, max_bytes - 3);
            /* 去掉截断处末尾的空格和逗号，再补省略号 */
            while (n) {
                if (s[n - 1] == ' ' || s[n - 1] == ',') {
                    n--;
                } else if (n >= 3 && memcmp(s + n - 3, "，", 3) == 0) {
                    n -= 3;
                } else if (n >= 3 && memcmp(s + n - 3, "、", 3) == 0) {
                    n -= 3;
                } else {
                    break;
                }
            }
            memcpy(out, s, n);
            memcpy(out + n, "…", 3);
            n += 3;
        } else {
            n = vc_utf8_cut(s, len, max_bytes);
            memcpy(out, s, n);
        }
    }
    out[n] = '\0';
    free(acc.buf);
    return n;
}
