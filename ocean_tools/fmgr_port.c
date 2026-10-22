/* --- fmgr port: shared buffers come from ot --- */
extern char tool_buf[];
static char tf_work[24576];

/* --- keycodes (kernel getch, verified) --- */
#define KB_TAB  0x09
#define KB_ENTER 0x0A
#define KB_BS   0x08
#define KB_F1   0x110
#define KB_F2   0x111
#define KB_F3   0x112
#define KB_F4   0x113
#define KB_F5   0x114
#define KB_F6   0x115
#define KB_F7   0x116
#define KB_F8   0x117
#define KB_INS  0x107
#define KB_DEL  0x106
#define KB_HOME 0x104
#define KB_END  0x105
#define KB_PGUP 0x108
#define KB_PGDN 0x109
#define KB_UP   0x100
#define KB_DOWN 0x101
#define KB_LEFT 0x102
#define KB_RIGHT 0x103
#define KB_CTRL(c) (((c) | 0x20) & 0x1F)
#define KB_CTRL_Q KB_CTRL('q')

/*
 * fmgr.c - CatOS v0.2 全屏文件管理器
 * 功能：目录浏览 / F3 查看 / F5 复制 / F6 RLE 压缩解压 / F7 归档解包 /
 *       F8 查找（文件名通配 + 内容搜索）/ Space 标记
 * 渲染：cls 整屏重绘（无 gotoxy API），选中行用亮色 + ">" 前缀标识。
 * 限制（实测）：cat() 一次最多 8192B（单文件查看/复制/归档源 ≤8KB）；
 *   save() ≤512B 整覆盖（产物一律分片 .Z00…/.A00…）；无 delete/rename（不做删除/移动）。
 */


#define LIST_ROWS 21

struct tf_dent { u8 is_dir; u8 marked; u32 size; char name[13]; };

static struct tf_dent tf_list[256];
static u32 tf_count = 0;
static char tf_path[64] = "/";
static u32 tf_sel = 0;      /* 光标 */
static u32 tf_top = 0;      /* 滚动顶 */
static char tf_notice[64];

/* ---------- 文件工具 ---------- */
static int tf_is_dir_ent(void) { return tf_sel < tf_count && tf_list[tf_sel].is_dir; }

static const char *tf_type_name(const struct tf_dent *d)
{
    if (d->is_dir) return "dir";
    {
        const char *n = d->name;
        u32 l = my_strlen(n);
        if (l >= 4 && my_strcmp(n + l - 4, ".Z00") == 0) return "zip";
        if (l >= 4 && my_strcmp(n + l - 4, ".A00") == 0) return "arc";
        if (l >= 4 && (my_strcmp(n + l - 4, ".TXT") == 0 || my_strcmp(n + l - 4, ".C") == 0 ||
                       my_strcmp(n + l - 4, ".MD") == 0 || my_strcmp(n + l - 4, ".LOG") == 0 ||
                       my_strcmp(n + l - 4, ".INI") == 0)) return "text";
        if (l >= 3 && my_strcmp(n + l - 3, ".CE") == 0) return "CE-app";
        if (l >= 4 && (my_strcmp(n + l - 4, ".ELF") == 0 || my_strcmp(n + l - 4, ".BIN") == 0)) return "bin";
    }
    return "file";
}

/* ---------- 路径工具 ---------- */
static void tf_path_join(const char *name, char *out, u32 outsz)
{
    (void)outsz;
    my_strcpy(out, tf_path);
    if (my_strcmp(tf_path, "/") != 0) my_strcat(out, "/");
    my_strcat(out, name);
}

static void tf_path_up(void)
{
    if (my_strcmp(tf_path, "/") == 0) return;
    char *p = tf_path + my_strlen(tf_path);
    if (p > tf_path && p[-1] == '/') p--;
    while (p > tf_path && p[-1] != '/') p--;
    if (p <= tf_path + 1) { my_strcpy(tf_path, "/"); }
    else { p[0] = 0; }
}

/* ---------- 列表收集 ---------- */
static int tf_ls_cb(int is_dir, const char *name, u32 size, void *ud)
{
    (void)ud;
    if (tf_count < 256) {
        u32 i;
        for (i = 0; i < tf_count; i++) {
            if (my_strcmp(tf_list[i].name, name) == 0) return 0;
        }
        my_strcpy(tf_list[tf_count].name, name);
        tf_list[tf_count].is_dir = (u8)is_dir;
        tf_list[tf_count].marked = 0;
        tf_list[tf_count].size = size;
        tf_count++;
    }
    return 0;
}

/* 冒泡排序：目录在前，然后名称升序 */
static void tf_sort_list(void)
{
    u32 i, j;
    for (i = 0; i < tf_count; i++) {
        for (j = i + 1; j < tf_count; j++) {
            int swap = 0;
            if (tf_list[j].is_dir && !tf_list[i].is_dir) swap = 1;
            else if (tf_list[j].is_dir == tf_list[i].is_dir &&
                     my_strcmp(tf_list[j].name, tf_list[i].name) < 0) swap = 1;
            if (swap) {
                struct tf_dent t = tf_list[i];
                tf_list[i] = tf_list[j];
                tf_list[j] = t;
            }
        }
    }
}

static void tf_load_dir(void)
{
    tf_count = 0;
    api->ls(tf_path, (void*)tf_ls_cb, 0);
    tf_sort_list();
    if (tf_sel >= tf_count) tf_sel = tf_count ? tf_count - 1 : 0;
    if (tf_top > tf_sel) tf_top = tf_sel;
    if (tf_sel >= tf_top + LIST_ROWS) tf_top = tf_sel - LIST_ROWS + 1;
}

/* ---------- 通配匹配 ---------- */
static int tf_wild_match2(const char *p, const char *s)
{
    for (;;) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            for (; *s; s++) if (tf_wild_match2(p, s)) return 1;
            return tf_wild_match2(p, s);
        }
        if (!*s) return *p == 0;
        if (*p == '?') { p++; s++; continue; }
        if (*p != *s) return 0;
        p++; s++;
    }
}

/* ---------- 渲染 ---------- */
static void tf_put_name(const char *s, u32 w)
{
    u32 k = 0;
    while (s[k] && k < w) { api->putc(s[k]); k++; }
    while (k < w) { api->putc(' '); k++; }
}

static void tf_put_u32w(u32 v, u32 w)
{
    char num[12];
    u32_to_dec(v, num);
    u32 l = my_strlen(num);
    u32 k;
    if (l > w) l = w;
    for (k = 0; k < w - l; k++) api->putc(' ');
    for (k = 0; k < l; k++) api->putc(num[k]);
}

static void tf_refresh(void)
{
    u32 i;
    api->cls();
    /* 标题栏 */
    api->set_fg(COL_LCYAN);
    out("  dir: ");
    out(tf_path);
    u32 k;
    for (k = 0; k < 22; k++) api->putc(' ');
    out("file manager");
    outln("");
    api->set_fg(COL_LGRAY);

    /* 列表 (a2: selected row rendered as inverse video 0x70) */
    for (i = 0; i < LIST_ROWS; i++) {
        u32 idx = tf_top + i;
        if (idx >= tf_count) { outln(""); continue; }
        struct tf_dent *d = &tf_list[idx];
        int sel = (idx == tf_sel);
        if (sel) api->set_fg(0x70);   /* light-gray background, black text */
        /* 选中标识 */
        if (sel) out(">");
        else out(" ");
        /* 标记 */
        if (!sel) api->set_fg(d->marked ? COL_LMAGENTA : COL_DGRAY);
        out(d->marked ? "*" : " ");
        /* 名称 */
        if (!sel) {
            if (d->is_dir) api->set_fg(COL_CYAN);
            else if (tf_type_name(d)[0] == 'C' && my_strlen(tf_type_name(d)) >= 3) api->set_fg(COL_LGREEN);
            else if (my_strcmp(tf_type_name(d), "zip") == 0 || my_strcmp(tf_type_name(d), "arc") == 0) api->set_fg(COL_YELLOW);
            else api->set_fg(COL_LGRAY);
        }
        out(" ");
        tf_put_name(d->name, 14);
        /* 大小 */
        if (!sel) api->set_fg(COL_LGRAY);
        if (d->is_dir) { tf_put_u32w(0, 10); }
        else tf_put_u32w(d->size, 10);
        out(" ");
        /* 类型 */
        if (!sel) api->set_fg(d->is_dir ? COL_CYAN : COL_BROWN);
        tf_put_name(tf_type_name(d), 10);
        if (!sel) api->set_fg(COL_LGRAY);
        outln("");
    }

    /* 状态栏 */
    api->set_fg(COL_LGREEN);
    out(" [");
    if (tf_count) {
        tf_put_u32w(tf_sel + 1, 0);
        out("/");
        tf_put_u32w(tf_count, 0);
        out("] ");
        out(tf_list[tf_sel].name);
    } else {
        out("empty dir");
    }
    api->set_fg(COL_LGRAY);
    u32 m = 0;
    for (i = 0; i < tf_count; i++) if (tf_list[i].marked) m++;
    if (m) { out("  marked "); out_u32(m); out(" item(s)"); }
    outln("");

    /* 提示条 (a2: <=79 cols, no wrap) */
    api->set_fg(COL_DGRAY);
    outln(" F1 help F3 view F5 copy F6 zip F7 arc F8 find Space mark Ctrl+Q quit");
    api->set_fg(COL_LGRAY);

    if (tf_notice[0]) {
        api->set_fg(COL_YELLOW);
        out(tf_notice);
        outln("");
        tf_notice[0] = 0;
    }
}

/* ---------- 查看 ---------- */
static void tf_view_file(const char *path)
{
    char *buf = (char *)tool_buf;
    int n = api->cat(path, buf, sizeof(buf));
    if (n < 0) { my_strcpy(tf_notice, "cannot read file"); return; }
    api->cls();
    api->set_fg(COL_LCYAN);
    out("---- "); out(path);
    out(" ("); out_u32((u32)n); outln(" bytes) ----");
    api->set_fg(COL_LGRAY);
    buf[n] = 0;
    out(buf);
    if (n > 0 && buf[n-1] != '\n') out("\n");
    out("--- any key to return ---");
    api->getch();
}

/* ---------- 复制（≤8KB，分片写） ---------- */
static int tf_save_shards_path(const char *path, const char *data, u32 len)
{
    u32 off = 0;
    int idx = 0;
    for (;;) {
        u32 chunk = len - off;
        if (chunk > 512) chunk = 512;
        char p2[64];
        char num[3];
        my_strcpy(p2, path);
        my_strcat(p2, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(p2, num);
        if (api->save(p2, data + off, chunk) < 0) return 0;
        off += chunk;
        idx++;
        if (off >= len) break;
    }
    return 1;
}

static void tf_copy_sel(void)
{
    if (tf_is_dir_ent()) { my_strcpy(tf_notice, "cannot copy dir (no recursive)"); return; }
    const char *src = tf_list[tf_sel].name;
    if (tf_list[tf_sel].size > 8192) { my_strcpy(tf_notice, "src >8KB, cannot copy (cat limit)"); return; }
    /* 目标名：前 5 字符 + ~1 + 扩展名 */
    char dst[64];
    u32 l = my_strlen(src);
    u32 i, k = 0;
    for (i = 0; i < l && k < 5 && src[i] != '.'; i++) dst[k++] = src[i];
    dst[k++] = '~'; dst[k++] = '1';
    for (; i < l; i++) dst[k++] = src[i];
    dst[k] = 0;
    char full[64];
    tf_path_join(dst, full, sizeof(full));
    char *buf = (char *)tool_buf;
    int n = api->cat(src, buf, sizeof(buf));
    if (n < 0) { my_strcpy(tf_notice, "read failed"); return; }
    if (tf_save_shards_path(full, buf, (u32)n)) {
        my_strcpy(tf_notice, "copied: ");
        my_strcat(tf_notice, dst);
    } else {
        my_strcpy(tf_notice, "copy failed (disk write)");
    }
}

/* ---------- RLE 压缩/解压 ---------- */
static void tf_rle_compress(const char *src_path, const char *dst_base)
{
    char *buf = (char *)tool_buf;
    int n = api->cat(src_path, buf, sizeof(buf));
    if (n < 0) { my_strcpy(tf_notice, "zip: read failed"); return; }
    /* 输出缓冲：最坏 3B/字节 → 24KB；bss 用 24KB */
    u32 o = 0;
    u32 i = 0;
    tf_work[o++] = 'F'; tf_work[o++] = 'Z'; tf_work[o++] = '0'; tf_work[o++] = '1';
    tf_work[o++] = (char)(n & 0xFF);
    tf_work[o++] = (char)((n >> 8) & 0xFF);
    tf_work[o++] = (char)((n >> 16) & 0xFF);
    tf_work[o++] = (char)((n >> 24) & 0xFF);
    while (i < (u32)n) {
        u8 b = (u8)buf[i];
        u32 c = 1;
        while (i + c < (u32)n && (u8)buf[i + c] == b && c < 255) c++;
        if (o + 3 > 24576) { my_strcpy(tf_notice, "zip buf too small"); return; }
        tf_work[o++] = (char)0xFE;
        tf_work[o++] = (char)c;
        tf_work[o++] = (char)b;
        i += c;
    }
    if (tf_save_shards_path(dst_base, tf_work, o)) {
        my_strcpy(tf_notice, "zipped: ");
        my_strcat(tf_notice, dst_base);
        my_strcat(tf_notice, " (.Z00…)");
    } else my_strcpy(tf_notice, "zip write failed");
}

static void tf_rle_decompress(const char *src_base, const char *dst_path)
{
    u32 ilen = 0;
    u32 idx = 0;
    for (;;) {
        char p[64];
        char num[3];
        my_strcpy(p, src_base);
        my_strcat(p, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(p, num);
        if (!api->file_exists(p)) break;
        int n = api->cat(p, tf_work + ilen, 24576 - ilen);
        if (n < 0) { my_strcpy(tf_notice, "unzip read failed"); return; }
        ilen += (u32)n;
        idx++;
        if (idx > 60) break;
    }
    if (ilen < 8 || !(tf_work[0] == 'F' && tf_work[1] == 'Z' && tf_work[2] == '0' && tf_work[3] == '1')) {
        my_strcpy(tf_notice, "not FZ01 zip file");
        return;
    }
    u32 olen = (u32)(u8)tf_work[4] | ((u32)(u8)tf_work[5] << 8) | ((u32)(u8)tf_work[6] << 16) | ((u32)(u8)tf_work[7] << 24);
    u32 o = 0;
    u32 i = 8;
    while (i < ilen && o < 24576) {
        u8 b = (u8)tf_work[i++];
        if (b == 0xFE && i + 2 <= ilen) {
            u32 c = (u8)tf_work[i++];
            u8 v = (u8)tf_work[i++];
            u32 k;
            for (k = 0; k < c && o < 24576; k++) tf_work[o++] = (char)v;
        } else if (b == 0xFE) {
            break;
        } else {
            tf_work[o++] = (char)b;
        }
    }
    if (o != olen) {
        my_strcpy(tf_notice, "unzip length mismatch");
        return;
    }
    if (tf_save_shards_path(dst_path, tf_work, o)) {
        my_strcpy(tf_notice, "unzipped: ");
        my_strcat(tf_notice, dst_path);
    } else my_strcpy(tf_notice, "unzip write failed");
}

/* ---------- 归档/解包 ---------- */
static void tf_do_archive(void)
{
    /* 收集已标记文件（或全部非目录） */
    u32 i;
    u32 nf = 0;
    struct tf_dent *sel[16];
    for (i = 0; i < tf_count && nf < 16; i++) {
        if (tf_list[i].is_dir) continue;
        if (tf_list[i].marked || (nf == 0 && tf_list[i].marked == 0 && i == tf_sel)) {
            if (tf_list[i].marked || i == tf_sel) { sel[nf++] = &tf_list[i]; }
        }
    }
    /* 修正：如果没有任何标记，用光标选中项 */
    u32 marked_n = 0;
    for (i = 0; i < tf_count; i++) if (tf_list[i].marked) marked_n++;
    if (marked_n == 0 && tf_sel < tf_count && !tf_list[tf_sel].is_dir) {
        nf = 1;
        sel[0] = &tf_list[tf_sel];
    }
    if (nf == 0) { my_strcpy(tf_notice, "arc: mark files with Space first"); return; }

    u32 pos = 0;
    /* 头 */
    tf_work[pos++] = 'F'; tf_work[pos++] = 'A'; tf_work[pos++] = 'R'; tf_work[pos++] = 'C';
    tf_work[pos++] = (char)(nf & 0xFF);
    tf_work[pos++] = (char)((nf >> 8) & 0xFF);
    /* 目录项占位：先写 0，回填 */
    u32 dir_off = pos;
    u32 k;
    for (k = 0; k < (u32)nf; k++) {
        u32 m;
        for (m = 0; m < 16; m++) tf_work[pos++] = 0;
        tf_work[pos++] = 0; tf_work[pos++] = 0; tf_work[pos++] = 0; tf_work[pos++] = 0;  /* size */
        tf_work[pos++] = 0; tf_work[pos++] = 0; tf_work[pos++] = 0; tf_work[pos++] = 0;  /* offset */
    }
    /* 数据区：逐文件 cat */
    for (i = 0; i < nf; i++) {
        u32 m;
        char nm[16];
        u32 nl = my_strlen(sel[i]->name);
        if (nl > 15) nl = 15;
        for (m = 0; m < nl; m++) nm[m] = sel[i]->name[m];
        for (; m < 16; m++) nm[m] = 0;
        char *buf = (char *)tool_buf;
        int n = api->cat(sel[i]->name, buf, sizeof(buf));
        if (n < 0) { my_strcpy(tf_notice, "arc: read failed"); return; }
        if (pos + (u32)n > 24576) { my_strcpy(tf_notice, "arc too big for buf(16KB)"); return; }
        /* 回填目录项 */
        u32 eo = dir_off + i * 24;
        for (m = 0; m < 16; m++) tf_work[eo + m] = nm[m];
        tf_work[eo+16] = (char)(n & 0xFF);
        tf_work[eo+17] = (char)((n >> 8) & 0xFF);
        tf_work[eo+18] = (char)((n >> 16) & 0xFF);
        tf_work[eo+19] = (char)((n >> 24) & 0xFF);
        tf_work[eo+20] = (char)(pos & 0xFF);
        tf_work[eo+21] = (char)((pos >> 8) & 0xFF);
        tf_work[eo+22] = (char)((pos >> 16) & 0xFF);
        tf_work[eo+23] = (char)((pos >> 24) & 0xFF);
        for (m = 0; m < (u32)n; m++) tf_work[pos++] = buf[m];
    }
    /* 写归档分片 */
    char dst[64];
    my_strcpy(dst, tf_path);
    if (my_strcmp(tf_path, "/") != 0) my_strcat(dst, "/");
    my_strcat(dst, "ARC1");
    if (tf_save_shards_path(dst, tf_work, pos)) {
        my_strcpy(tf_notice, "archived ");
        out_u32(nf);
        my_strcat(tf_notice, " file(s) -> ARC1.A00...");
        /* 清除标记 */
        for (i = 0; i < tf_count; i++) tf_list[i].marked = 0;
    } else my_strcpy(tf_notice, "arc write failed");
}

static void tf_do_extract(const char *src_base)
{
    u32 ilen = 0;
    u32 idx = 0;
    for (;;) {
        char p[64];
        char num[3];
        my_strcpy(p, src_base);
        my_strcat(p, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(p, num);
        if (!api->file_exists(p)) break;
        int n = api->cat(p, tf_work + ilen, 24576 - ilen);
        if (n < 0) { my_strcpy(tf_notice, "unarc read failed"); return; }
        ilen += (u32)n;
        idx++;
        if (idx > 60) break;
    }
    if (ilen < 6 || !(tf_work[0] == 'F' && tf_work[1] == 'A' && tf_work[2] == 'R' && tf_work[3] == 'C')) {
        my_strcpy(tf_notice, "not FARC archive");
        return;
    }
    u32 nf = (u32)(u8)tf_work[4] | ((u32)(u8)tf_work[5] << 8);
    u32 i;
    for (i = 0; i < nf; i++) {
        u32 eo = 6 + i * 24;
        if (eo + 24 > ilen) { my_strcpy(tf_notice, "arc entry out of range"); return; }
        char nm[17];
        u32 m;
        for (m = 0; m < 16; m++) nm[m] = tf_work[eo + m];
        nm[16] = 0;
        /* 去除尾部空格/NUL */
        while (m > 0 && (nm[m-1] == ' ' || nm[m-1] == 0)) m--;
        nm[m] = 0;
        u32 fsz = (u32)(u8)tf_work[eo+16] | ((u32)(u8)tf_work[eo+17] << 8) | ((u32)(u8)tf_work[eo+18] << 16) | ((u32)(u8)tf_work[eo+19] << 24);
        u32 fof = (u32)(u8)tf_work[eo+20] | ((u32)(u8)tf_work[eo+21] << 8) | ((u32)(u8)tf_work[eo+22] << 16) | ((u32)(u8)tf_work[eo+23] << 24);
        if (fof + fsz > ilen) { my_strcpy(tf_notice, "arc data out of range"); return; }
        /* 8.3 截断 + 冲突改名 */
        char target[64];
        my_strcpy(target, tf_path);
        if (my_strcmp(tf_path, "/") != 0) my_strcat(target, "/");
        my_strcat(target, nm);
        if (api->file_exists(target)) {
            /* 改名 名~1.扩展 */
            char base2[6];
            u32 tl = my_strlen(nm);
            u32 k;
            for (k = 0; k < 5 && nm[k] && nm[k] != '.'; k++) base2[k] = nm[k];
            base2[k] = 0;
            my_strcpy(target, tf_path);
            if (my_strcmp(tf_path, "/") != 0) my_strcat(target, "/");
            my_strcat(target, base2);
            my_strcat(target, "~1");
            for (; k < tl && nm[k]; k++) my_strcat(target, (char[2]){ nm[k], 0 });
        }
        tf_save_shards_path(target, tf_work + fof, fsz);
        out("extracted: "); outln(nm);
    }
    api->getch();
    my_strcpy(tf_notice, "unarc done");
}

/* ---------- 查找 ---------- */
static void tf_do_find(void)
{
    char pat[64];
    pat[0] = 0;
    api->cls();
    api->set_fg(COL_LCYAN);
    outln("find (c: prefix = content search, else name wildcard * ?):");
    api->set_fg(COL_LGRAY);
    out("pattern> ");
    if (api->readline(pat, 60) <= 0) return;
    if (pat[0] == 'c' && pat[1] == ':') {
        /* 内容搜索 */
        const char *kw = pat + 2;
        u32 i, hits = 0;
        for (i = 0; i < tf_count; i++) {
            if (tf_list[i].is_dir) continue;
            if (tf_list[i].size > 8192) continue;
            char *buf = (char *)tool_buf;
            int n = api->cat(tf_list[i].name, buf, sizeof(buf));
            if (n < 0) continue;
            buf[n] = 0;
            u32 line = 1;
            u32 k = 0;
            while (k < (u32)n) {
                u32 s = k;
                while (k < (u32)n && buf[k] != '\n') k++;
                /* 行内容 [s,k) */
                u32 m;
                int hit = 0;
                for (m = s; m + my_strlen(kw) <= k; m++) {
                    u32 q;
                    for (q = 0; kw[q]; q++) {
                        if (my_toupper(buf[m+q]) != my_toupper(kw[q])) break;
                    }
                    if (!kw[q]) { hit = 1; break; }
                }
                if (hit) {
                    api->set_fg(COL_LGREEN);
                    out(tf_list[i].name); out(":"); out_u32(line); out(": ");
                    u32 p;
                    for (p = s; p < k && p < s + 60; p++) api->putc(buf[p]);
                    outln("");
                    hits++;
                }
                line++;
                if (k < (u32)n) k++;
            }
        }
        api->set_fg(COL_LGRAY);
        out("content search done, "); out_u32(hits); outln(" hit(s)");
        api->getch();
        return;
    }
    /* 文件名通配：过滤显示 */
    api->cls();
    api->set_fg(COL_LCYAN);
    out("match "); out(pat); outln(":");
    api->set_fg(COL_LGRAY);
    u32 i, hits = 0;
    for (i = 0; i < tf_count; i++) {
        if (tf_wild_match2(pat, tf_list[i].name)) {
            out(tf_list[i].name);
            outln("");
            hits++;
        }
    }
    out("total "); out_u32(hits); outln(" matched");
    api->getch();
}

/* ---------- 帮助 ---------- */
static void tf_do_help(void)
{
    api->cls();
    api->set_fg(COL_LCYAN);
    outln("file manager hotkey help");
    api->set_fg(COL_LGRAY);
    outln(" Enter      enter dir");
    outln(" Backspace  up dir");
    outln(" Arrows/PgUp/PgDn move/page");
    outln(" F3         view file");
    outln(" F5         copy file (<=8KB, shards)");
    outln(" F6         zip file (.Z) / unzip .Z");
    outln(" F7         archive marked -> ARC1.A00... / unarc");
    outln(" F8         find: name wildcard or c:content");
    outln(" Space      mark/unmark (for arc)");
    outln(" Ctrl+Q     quit");
    outln(" limits: no del/rename API; cat 8KB; save 512B/shard");
    outln(" --- any key to return ---");
    api->getch();
}

/* ---------- 入口 ---------- */
static void tfm_main(void)
{
    tf_path[0] = '/'; tf_path[1] = 0;
    tf_notice[0] = 0;
    tf_load_dir();
    tf_refresh();
    for (;;) {
        int k = api->getch();
        if (k == KB_CTRL_Q) break;
        if (k == KB_UP) {
            if (tf_sel > 0) tf_sel--;
            if (tf_sel < tf_top) tf_top = tf_sel;
            tf_refresh();
        } else if (k == KB_DOWN) {
            if (tf_sel + 1 < tf_count) tf_sel++;
            if (tf_sel >= tf_top + LIST_ROWS) tf_top = tf_sel - LIST_ROWS + 1;
            tf_refresh();
        } else if (k == KB_PGUP) {
            u32 pg = LIST_ROWS;
            tf_sel = tf_sel > pg ? tf_sel - pg : 0;
            if (tf_sel < tf_top) tf_top = tf_sel;
            tf_refresh();
        } else if (k == KB_PGDN) {
            u32 pg = LIST_ROWS;
            tf_sel += pg;
            if (tf_sel >= tf_count) tf_sel = tf_count ? tf_count - 1 : 0;
            if (tf_sel >= tf_top + LIST_ROWS) tf_top = tf_sel - LIST_ROWS + 1;
            tf_refresh();
        } else if (k == KB_ENTER || k == 0x0D || k == 0x0A) {
            if (tf_count && tf_sel < tf_count && tf_list[tf_sel].is_dir) {
                char np[64];
                tf_path_join(tf_list[tf_sel].name, np, sizeof(np));
                my_strcpy(tf_path, np);
                tf_load_dir();
                tf_refresh();
            }
        } else if (k == KB_BS || k == KB_ESC) {
            if (my_strcmp(tf_path, "/") != 0) {
                tf_path_up();
                tf_load_dir();
                tf_refresh();
            }
        } else if (k == KB_F1) {
            tf_do_help();
            tf_refresh();
        } else if (k == KB_F3) {
            if (tf_count && tf_sel < tf_count && !tf_list[tf_sel].is_dir) {
                char fp[64];
                tf_path_join(tf_list[tf_sel].name, fp, sizeof(fp));
                tf_view_file(fp);
                tf_refresh();
            }
        } else if (k == KB_F5) {
            if (tf_count && tf_sel < tf_count) { tf_copy_sel(); tf_refresh(); }
        } else if (k == KB_F6) {
            if (tf_count && tf_sel < tf_count && !tf_list[tf_sel].is_dir) {
                char fp[64];
                char base[64];
                const char *nm = tf_list[tf_sel].name;
                u32 l = my_strlen(nm);
                if (l >= 4 && my_strcmp(nm + l - 4, ".Z00") == 0) {
                    /* 解压：目标 = 原名去 .Z00 前部分 */
                    u32 i;
                    for (i = 0; i < l - 4 && i < 60; i++) base[i] = nm[i];
                    base[i] = 0;
                    /* 去掉可能残留的 ~1 */
                    my_strcpy(fp, tf_path);
                    if (my_strcmp(tf_path, "/") != 0) my_strcat(fp, "/");
                    my_strcat(fp, base);
                    tf_rle_decompress(fp, fp);
                } else {
                    tf_path_join(nm, fp, sizeof(fp));
                    my_strcpy(base, tf_path);
                    if (my_strcmp(tf_path, "/") != 0) my_strcat(base, "/");
                    u32 i2;
                    u32 nl = my_strlen(nm);
                    for (i2 = 0; i2 < nl && nm[i2] != '.' && i2 < 5; i2++) base[my_strlen(base)] = nm[i2];
                    my_strcat(base, "Z");
                    tf_rle_compress(fp, base);
                }
                tf_load_dir();
                tf_refresh();
            }
        } else if (k == KB_F7) {
            if (tf_count && tf_sel < tf_count) {
                const char *nm = tf_list[tf_sel].name;
                u32 l = my_strlen(nm);
                if (l >= 4 && my_strcmp(nm + l - 4, ".A00") == 0) {
                    char base[64];
                    u32 i2;
                    for (i2 = 0; i2 < l - 4 && i2 < 60; i2++) base[i2] = nm[i2];
                    base[i2] = 0;
                    char fp[64];
                    my_strcpy(fp, tf_path);
                    if (my_strcmp(tf_path, "/") != 0) my_strcat(fp, "/");
                    my_strcat(fp, base);
                    tf_do_extract(fp);
                } else {
                    tf_do_archive();
                }
                tf_load_dir();
                tf_refresh();
            }
        } else if (k == KB_F8) {
            tf_do_find();
            tf_refresh();
        } else if (k == ' ') {
            if (tf_sel < tf_count) {
                tf_list[tf_sel].marked = !tf_list[tf_sel].marked;
                tf_refresh();
            }
        }
    }
}
