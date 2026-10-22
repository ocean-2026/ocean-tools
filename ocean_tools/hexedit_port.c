
/* --- keycodes (kernel getch, verified by probe) --- */
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
#define KB_CTRL_S KB_CTRL('s')
#define KB_CTRL_Z KB_CTRL('z')
#define KB_CTRL_Y KB_CTRL('y')
#define KB_CTRL_O KB_CTRL('o')
#define KB_CTRL_G KB_CTRL('g')

static void u32_to_hex(u32 v, char *out) {
    static const char hd[] = "0123456789abcdef";
    u32 i;
    for (i = 0; i < 8; i++) out[i] = hd[(v >> (28 - i * 4)) & 15];
    out[8] = 0;
}
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
/*
 * hexedit.c - CatOS v0.2 更好的编辑器（文本/十六进制双模式）
 * 功能：文本编辑 / F2 十六进制编辑 / Ctrl+O 特殊字符显示 / 语法高亮(C/Py/ASM) /
 *       行号 / 撤销重做 / 状态栏 / 分片保存。
 * 渲染：cls 整屏重绘（无 gotoxy API），光标行用亮色标识。
 * 限制（实测）：cat() 一次最多读 8192B（编辑文件 ≤8KB）；save() ≤512B/片；
 *   无 printf（数字格式化自写）。
 */


#define BUF_MAX 8192
#define MAX_LINES 512

static u8 g_buf[BUF_MAX];
static u32 g_len = 0;
static u32 g_line_off[MAX_LINES];
static u16 g_line_len[MAX_LINES];
static u32 g_nlines = 0;

static char g_fname[64] = "new.txt";
static int g_dirty = 0;
static int g_mode = 0;        /* 0=文本 1=十六进制 */
static int g_special = 0;     /* 特殊字符显示 */
static int g_lineno = 1;      /* 行号显示 */
static int g_ins = 1;         /* 1=插入 0=覆盖 */

/* 光标：文本 (g_row, g_col)；hex (g_hpos 字节偏移) */
static u32 g_row = 0, g_col = 0;
static u32 g_hpos = 0;
static u32 g_top = 0;         /* 文本滚动顶行 */
static u32 g_hex_top = 0;     /* hex 滚动顶部字节 */

/* 语法高亮跨行状态 */
static int g_in_comment = 0;

static void rebuild_lines(void);

/* ---------- 撤销栈 ---------- */
struct undo_rec {
    u32 pos;      /* 字节偏移 */
    u16 old_len;
    u16 new_len;
    u8 old_data[64];
    u8 new_data[64];
};
static struct undo_rec g_undo[128];
static u32 g_undo_head = 0;   /* 写入位置 */
static u32 g_undo_count = 0;  /* 有效条目 */
static u32 g_redo_n = 0;      /* 可重做条数 */

static void undo_push(u32 pos, const u8 *oldd, u16 oldlen, const u8 *newd, u16 newlen)
{
    struct undo_rec *r = &g_undo[g_undo_head];
    u32 i;
    r->pos = pos;
    r->old_len = oldlen;
    r->new_len = newlen;
    for (i = 0; i < oldlen && i < 64; i++) r->old_data[i] = oldd[i];
    for (i = 0; i < newlen && i < 64; i++) r->new_data[i] = newd[i];
    g_undo_head = (g_undo_head + 1) % 128;
    if (g_undo_count < 128) g_undo_count++;
    g_redo_n = 0;   /* 新操作清除重做分支 */
}

/* 撤销/重做 */
static void do_undo(int redo)
{
    if (g_undo_count == 0) return;
    if (redo) {
        if (g_redo_n == 0) return;
        /* 重做：应用 g_undo[head]（撤销后 head 正指向被撤销记录） */
        struct undo_rec *r = &g_undo[g_undo_head];
        u32 i;
        if (r->old_len) {
            for (i = r->pos; i + r->old_len < g_len; i++) g_buf[i] = g_buf[i + r->old_len];
            g_len -= r->old_len;
        }
        if (r->new_len) {
            for (i = g_len + r->new_len; i > r->pos + r->new_len; i--) g_buf[i - 1] = g_buf[i - r->new_len - 1];
            for (i = 0; i < r->new_len; i++) g_buf[r->pos + i] = r->new_data[i];
            g_len += r->new_len;
        }
        g_undo_head = (g_undo_head + 1) % 128;
        g_undo_count++;
        g_redo_n--;
        g_dirty = 1;
    } else {
        /* 撤销最后一条 */
        u32 idx = (g_undo_head + 127) % 128;
        struct undo_rec *r = &g_undo[idx];
        u32 i;
        /* 移除 new 部分，插回 old 部分 */
        if (r->new_len) {
            for (i = r->pos; i + r->new_len < g_len; i++) g_buf[i] = g_buf[i + r->new_len];
            g_len -= r->new_len;
        }
        if (r->old_len) {
            for (i = g_len + r->old_len; i > r->pos + r->old_len; i--) g_buf[i - 1] = g_buf[i - r->old_len - 1];
            for (i = 0; i < r->old_len; i++) g_buf[r->pos + i] = r->old_data[i];
            g_len += r->old_len;
        }
        g_undo_head = idx;
        g_undo_count--;
        g_redo_n++;
        g_dirty = 1;
    }
    rebuild_lines();
}

/* ---------- 行表重建 ---------- */
static void rebuild_lines(void)
{
    u32 i = 0;
    g_nlines = 0;
    while (i < g_len && g_nlines < MAX_LINES) {
        g_line_off[g_nlines] = i;
        u32 s = i;
        while (i < g_len && g_buf[i] != '\n') i++;
        g_line_len[g_nlines] = (u16)(i - s);
        g_nlines++;
        if (i < g_len) i++;
    }
    if (g_len > 0 && g_nlines == 0) { /* 空文件至少 1 行 */ }
}

/* 行/列 → 字节偏移 */
static u32 row_col_to_off(u32 row, u32 col)
{
    if (row >= g_nlines) return g_len;
    u32 off = g_line_off[row];
    if (col > g_line_len[row]) col = g_line_len[row];
    return off + col;
}

/* 偏移 → 行/列 */
static void off_to_row_col(u32 off, u32 *row, u32 *col)
{
    u32 r;
    for (r = 0; r < g_nlines; r++) {
        u32 start = g_line_off[r];
        u32 end = start + g_line_len[r];
        if (off <= end) { *row = r; *col = off - start; return; }
        /* 注意行分隔符：off 可能在 \n 位置（end+1）*/
        if (off == end + 1 && r + 1 < g_nlines) continue;
        if (off < start + g_line_len[r] + 1) { *row = r; *col = (off > start + g_line_len[r]) ? g_line_len[r] : off - start; return; }
    }
    *row = g_nlines ? g_nlines - 1 : 0;
    *col = *row < g_nlines ? g_line_len[*row] : 0;
}

/* ---------- 插入/删除 ---------- */
static void insert_at(u32 pos, u8 b)
{
    if (g_len >= BUF_MAX) return;
    u32 i;
    for (i = g_len; i > pos; i--) g_buf[i] = g_buf[i - 1];
    g_buf[pos] = b;
    g_len++;
    g_dirty = 1;
}

static void delete_at(u32 pos)
{
    if (pos >= g_len) return;
    u32 i;
    for (i = pos; i + 1 < g_len; i++) g_buf[i] = g_buf[i + 1];
    g_len--;
    g_dirty = 1;
}

static void replace_at(u32 pos, u8 b)
{
    if (pos < g_len) {
        g_buf[pos] = b;
        g_dirty = 1;
    }
}

/* ---------- 保存（分片） ---------- */
static int save_shards(void)
{
    u32 off = 0;
    int idx = 0;
    if (g_len <= 512) {
        /* small file: write the real name directly (no .A00 shard) */
        if (api->save(g_fname, (char *)g_buf, g_len) < 0) return 0;
        g_dirty = 0;
        return 1;
    }
    for (;;) {
        u32 chunk = g_len - off;
        if (chunk > 512) chunk = 512;
        char path[64];
        char num[3];
        my_strcpy(path, g_fname);
        my_strcat(path, ".A");
        num[0] = (char)('0' + idx / 10);
        num[1] = (char)('0' + idx % 10);
        num[2] = 0;
        my_strcat(path, num);
        if (api->save(path, (char*)g_buf + off, chunk) < 0) return 0;
        off += chunk;
        idx++;
        if (off >= g_len) break;
    }
    g_dirty = 0;
    return 1;
}

/* ---------- 语法高亮关键字表 ---------- */
static const char *c_keywords[] = {
    "int","char","void","long","short","unsigned","signed","float","double",
    "if","else","for","while","do","switch","case","default","break","continue",
    "return","struct","union","enum","typedef","static","extern","const",
    "volatile","register","sizeof","goto","main",0
};
static const char *py_keywords[] = {
    "def","class","if","elif","else","for","while","return","import","from",
    "pass","break","continue","try","except","finally","with","as","lambda",
    "print","None","True","False","and","or","not","in","is","global","raise",0
};
static const char *asm_keywords[] = {
    "mov","push","pop","call","ret","jmp","je","jne","jz","jnz","jb","ja",
    "cmp","test","add","sub","and","or","xor","inc","dec","int","lea","nop",
    "shl","shr","sar","imul","mul","div","loop","stosb","movsb","cld","std",0
};

static int kw_match(const char *word, const char *kw)
{
    u32 i = 0;
    while (word[i] && kw[i] && word[i] == kw[i]) i++;
    return word[i] == 0 && kw[i] == 0;
}

/* 语法类型：0 普通 1 C 2 Python 3 ASM（按扩展名，大小写不敏感） */
static int syn_type(const char *fname)
{
    const char *dot = my_strrchr(fname, '.');
    char e[8];
    u32 i;
    if (!dot) return 0;
    u32 l = my_strlen(dot + 1);
    for (i = 0; i < l && i < 7; i++) e[i] = my_toupper(dot[i + 1]);
    e[i] = 0;
    if (my_strcmp(e, "C") == 0 || my_strcmp(e, "H") == 0) return 1;
    if (my_strcmp(e, "PY") == 0) return 2;
    if (my_strcmp(e, "ASM") == 0 || my_strcmp(e, "S") == 0) return 3;
    return 0;
}

/* 渲染一行（文本模式，带高亮）；返回行是否含换行前内容 */
static void render_text_line(u32 row, int hl)
{
    const char **kws = 0;
    u32 l = g_line_len[row];
    u32 i;
    if (hl == 1) { kws = c_keywords; }
    else if (hl == 2) { kws = py_keywords; }
    else if (hl == 3) { kws = asm_keywords; }

    const u8 *ln = g_buf + g_line_off[row];
    /* 处理行尾 \r 记号（不影响显示长度） */
    u32 disp = l;
    if (disp > 0 && ln[disp-1] == '\r') disp--;

    i = 0;
    while (i < disp && i < 74) {
        u8 c = ln[i];
        int col = COL_LGRAY;
        if (hl) {
            if (g_in_comment) {
                /* 块注释状态：整段绿色显示 */
                col = COL_GREEN;
                if (i + 1 < disp && c == '/' && ln[i+1] == '*') {
                    col = COL_GREEN;
                }
                if (c == '*' && i + 1 < disp && ln[i+1] == '/') {
                    api->set_fg(COL_GREEN);
                    api->putc('*'); api->putc('/');
                    i += 2;
                    g_in_comment = 0;
                    continue;
                }
                api->set_fg(COL_GREEN);
                api->putc((char)c);
                i++;
                continue;
            }
            /* 字符串 */
            if (c == '"' || c == '\'') {
                col = COL_YELLOW;
                /* 输出到行尾或闭合 */
                api->set_fg(COL_YELLOW);
                api->putc((char)c);
                i++;
                while (i < disp) {
                    api->putc((char)ln[i]);
                    if (ln[i] == '\\' && i + 1 < disp) { api->putc((char)ln[++i]); i++; continue; }
                    if (ln[i] == c) { i++; break; }
                    i++;
                }
                continue;
            }
            /* 预处理（行首 #） */
            if (c == '#' && i == 0) {
                api->set_fg(COL_LCYAN);
                while (i < disp && i < 74) { api->putc((char)ln[i]); i++; }
                continue;
            }
            /* 注释 // */
            if (c == '/' && i + 1 < disp && ln[i+1] == '/') {
                api->set_fg(COL_GREEN);
                while (i < disp && i < 74) { api->putc((char)ln[i]); i++; }
                continue;
            }
            if (c == '/' && i + 1 < disp && ln[i+1] == '*') {
                api->set_fg(COL_GREEN);
                api->putc('/'); api->putc('*');
                i += 2;
                g_in_comment = 1;
                continue;
            }
            /* 标识符/关键字 */
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
                char word[24];
                u32 wl = 0;
                while (i + wl < disp && wl < 23 &&
                       ((ln[i+wl] >= 'a' && ln[i+wl] <= 'z') ||
                        (ln[i+wl] >= 'A' && ln[i+wl] <= 'Z') ||
                        (ln[i+wl] >= '0' && ln[i+wl] <= '9') || ln[i+wl] == '_')) {
                    word[wl] = (char)ln[i+wl];
                    wl++;
                }
                word[wl] = 0;
                int iskw = 0;
                u32 ki;
                for (ki = 0; kws && kws[ki]; ki++) {
                    if (kw_match(word, kws[ki])) { iskw = 1; break; }
                }
                if (iskw) api->set_fg(COL_LBLUE);
                else api->set_fg(COL_LGRAY);
                u32 w2;
                for (w2 = 0; w2 < wl; w2++) api->putc(word[w2]);
                i += wl;
                continue;
            }
            /* 数字 */
            if (c >= '0' && c <= '9') {
                col = COL_RED;
            }
        }
        api->set_fg(col);
        api->putc((char)c);
        i++;
    }
    /* 特殊字符显示：行尾 \r 记号 */
    if (g_special && l > 0 && ln[l-1] == '\r' && disp < 74) {
        api->set_fg(COL_LMAGENTA);
        api->putc(0x10);   /* ► 箭头记号 */
    }
    /* 行尾 \n 记号（文件最后一行无） */
    if (g_special && row + 1 < g_nlines && disp < 74) {
        api->set_fg(COL_LMAGENTA);
        api->putc(0x11);   /* ◄ */
    }
    api->set_fg(COL_LGRAY);
}

/* ---------- 渲染 ---------- */
static void refresh(void)
{
    u32 i;
    api->cls();
    /* 标题栏 */
    api->set_fg(COL_LCYAN);
    out("editpp  ");
    out(g_fname);
    out(g_mode ? "  [HEX]" : "  [TXT]");
    if (g_dirty) out(" [MOD]");
    if (g_special) out(" [SHOW-SPECIAL]");
    if (g_lineno) out(" [LINE-NO]");
    outln("");
    api->set_fg(COL_LGRAY);

    if (g_mode == 0) {
        /* 文本模式 */
        u32 rows = 22;
        for (i = 0; i < rows; i++) {
            u32 row = g_top + i;
            if (row >= g_nlines) { outln(""); continue; }
            if (g_lineno) {
                api->set_fg(row == g_row ? COL_WHITE : COL_DGRAY);
                char num[5];
                u32_to_dec(row + 1, num);
                u32 l = my_strlen(num);
                u32 k;
                for (k = l; k < 4; k++) api->putc(' ');
                out(num);
                api->putc('|');
            }
            if (row == g_row) api->set_fg(COL_WHITE);
            render_text_line(row, syn_type(g_fname));
            outln("");
        }
    } else {
        /* 十六进制模式 */
        u32 rows = 22;
        for (i = 0; i < rows; i++) {
            u32 base = g_hex_top + i * 16;
            if (base >= g_len) { outln(""); continue; }
            /* 偏移 */
            api->set_fg(COL_DGRAY);
            u32 v = base;
            char hx[9];
            u32_to_hex(v, hx);
            out(hx);
            api->putc(' ');
            /* hex */
            u32 j;
            for (j = 0; j < 16; j++) {
                u32 pos = base + j;
                if (pos < g_len) {
                    api->set_fg(pos == g_hpos ? COL_YELLOW : COL_LGRAY);
                    char h2[3];
                    u8 b = g_buf[pos];
                    h2[0] = "0123456789abcdef"[b >> 4];
                    h2[1] = "0123456789abcdef"[b & 15];
                    h2[2] = 0;
                    out(h2);
                } else {
                    out("  ");
                }
                api->putc(' ');
            }
            api->putc(' ');
            /* ascii */
            api->set_fg(COL_BROWN);
            for (j = 0; j < 16; j++) {
                u32 pos = base + j;
                if (pos < g_len) {
                    u8 c = g_buf[pos];
                    api->putc((c >= 32 && c < 127) ? (char)c : '.');
                } else api->putc(' ');
            }
            api->set_fg(COL_LGRAY);
            outln("");
        }
    }

    /* 状态栏 */
    api->set_fg(COL_LGREEN);
    if (g_mode == 0) {
        out(" row:"); out_u32(g_row + 1);
        out(" col:"); out_u32(g_col + 1);
    } else {
        out(" off:0x");
        {
            char hx[9];
            u32_to_hex(g_hpos, hx);
            out(hx);
        }
    }
    out("  bytes:"); out_u32(g_len);
    out("  "); out(g_ins ? "INS" : "OVR");
    api->set_fg(COL_LGRAY);
    u32 fill = 30;
    for (i = 0; i < fill; i++) api->putc(' ');
    api->set_fg(COL_DGRAY);
    outln("F2 hex  Ctrl+O special  Ctrl+S save");
    api->set_fg(COL_LGRAY);
}

/* ---------- 文件加载 ---------- */
static void load_file(const char *path)
{
    my_strcpy(g_fname, path);
    int n = api->cat(path, (char*)g_buf, sizeof(g_buf));
    if (n >= 0) {
        g_len = (u32)n;
        out("loaded "); out_u32(g_len); outln(" bytes");
    } else {
        g_len = 0;
        outln("new file (created on save)");
    }
    rebuild_lines();
    g_dirty = 0;
}

/* ---------- 帮助 ---------- */
static void do_help(void)
{
    api->cls();
    api->set_fg(COL_LCYAN);
    outln("editpp help");
    api->set_fg(COL_LGRAY);
    outln(" arrows/Home/End/PgUp/PgDn  move cursor");
    outln(" Insert       INS/OVR toggle");
    outln(" Delete/BS    delete char");
    outln(" F2           text/hex mode toggle");
    outln(" Ctrl+O       show special (\\n\\t\\r marks)");
    outln(" Ctrl+G       line numbers toggle");
    outln(" Ctrl+S       save (>512B splits .A00...)");
    outln(" Ctrl+Z/Y     undo/redo");
    outln(" Ctrl+Q       quit (confirm if dirty)");
    outln(" highlight: .c/.h blue kw .py .asm");
    outln(" limits: file <=8KB (cat); shard <=512B");
    outln(" --- any key to return ---");
    api->getch();
}

/* ---------- 入口 ---------- */
static void thex_main(const char *file_arg)
{
    if (file_arg && file_arg[0]) {
        load_file(file_arg);
    } else {
        my_strcpy(g_fname, "new.txt");
        g_len = 0;
        rebuild_lines();
        outln("new file new.txt (Ctrl+S save)");
    }
    refresh();
    for (;;) {
        int k = api->getch();
        if (k == KB_CTRL_Q) {
            if (g_dirty) {
                api->set_fg(COL_YELLOW);
                outln("unsaved! Ctrl+Q again to quit, other key cancels");
                api->set_fg(COL_LGRAY);
                int k2 = api->getch();
                if (k2 != KB_CTRL_Q) { refresh(); continue; }
            }
            break;
        }
        if (k == KB_CTRL_S) {
            if (!save_shards()) {
                api->set_fg(COL_LRED);   /* a2: unified light-red error */
                outln("save failed!");
                api->set_fg(COL_LGRAY);
            }
            refresh();
            continue;
        }
        if (k == KB_CTRL_Z) { do_undo(0); refresh(); continue; }
        if (k == KB_CTRL_Y) { do_undo(1); refresh(); continue; }
        if (k == KB_CTRL_O) { g_special = !g_special; refresh(); continue; }
        if (k == KB_CTRL_G) { g_lineno = !g_lineno; refresh(); continue; }
        if (k == KB_F2) { g_mode = !g_mode; refresh(); continue; }
        if (k == KB_F1) { do_help(); refresh(); continue; }
        if (k == KB_INS) { g_ins = !g_ins; refresh(); continue; }

        if (g_mode == 0) {
            /* 文本模式 */
            if (k == KB_UP) { if (g_row > 0) g_row--; if (g_row < g_top) g_top = g_row; if (g_col > g_line_len[g_row]) g_col = g_line_len[g_row]; refresh(); continue; }
            if (k == KB_DOWN) { if (g_row + 1 < g_nlines) g_row++; if (g_row >= g_top + 22) g_top = g_row - 21; if (g_col > g_line_len[g_row]) g_col = g_line_len[g_row]; refresh(); continue; }
            if (k == KB_LEFT) { if (g_col > 0) g_col--; else if (g_row > 0) { g_row--; g_col = g_line_len[g_row]; if (g_row < g_top) g_top = g_row; } refresh(); continue; }
            if (k == KB_RIGHT) { if (g_col < g_line_len[g_row]) g_col++; else if (g_row + 1 < g_nlines) { g_row++; g_col = 0; if (g_row >= g_top + 22) g_top = g_row - 21; } refresh(); continue; }
            if (k == KB_HOME) { g_col = 0; refresh(); continue; }
            if (k == KB_END) { g_col = g_line_len[g_row]; refresh(); continue; }
            if (k == KB_PGUP) { g_row = g_row > 22 ? g_row - 22 : 0; if (g_row < g_top) g_top = g_row; refresh(); continue; }
            if (k == KB_PGDN) { g_row += 22; if (g_row >= g_nlines) g_row = g_nlines ? g_nlines - 1 : 0; if (g_row >= g_top + 22) g_top = g_row - 21; refresh(); continue; }
            if (k == KB_ENTER || k == 0x0D || k == 0x0A) {
                u32 pos = row_col_to_off(g_row, g_col);
                u8 nl = '\n';
                undo_push(pos, 0, 0, &nl, 1);
                insert_at(pos, '\n');
                rebuild_lines();
                g_row++;
                g_col = 0;
                if (g_row >= g_top + 22) g_top = g_row - 21;
                refresh();
                continue;
            }
            if (k == KB_TAB) {
                u32 pos = row_col_to_off(g_row, g_col);
                u8 tb = '\t';
                undo_push(pos, 0, 0, &tb, 1);
                insert_at(pos, '\t');
                g_col++;
                rebuild_lines();
                refresh();
                continue;
            }
            if (k == KB_BS) {
                if (g_col > 0) {
                    u32 pos = row_col_to_off(g_row, g_col) - 1;
                    u8 old = g_buf[pos];
                    undo_push(pos, &old, 1, 0, 0);
                    delete_at(pos);
                    g_col--;
                    rebuild_lines();
                } else if (g_row > 0) {
                    u32 prev_len = g_line_len[g_row - 1];
                    u32 pos = row_col_to_off(g_row - 1, prev_len);
                    u8 old = g_buf[pos];
                    undo_push(pos, &old, 1, 0, 0);
                    delete_at(pos);
                    g_row--;
                    g_col = prev_len;
                    rebuild_lines();
                }
                refresh();
                continue;
            }
            if (k == KB_DEL) {
                u32 pos = row_col_to_off(g_row, g_col);
                if (pos < g_len) {
                    u8 old = g_buf[pos];
                    undo_push(pos, &old, 1, 0, 0);
                    delete_at(pos);
                    rebuild_lines();
                    if (g_col > g_line_len[g_row]) g_col = g_line_len[g_row];
                }
                refresh();
                continue;
            }
            if (k >= 0x20 && k < 0x7F) {
                u32 pos = row_col_to_off(g_row, g_col);
                u8 c = (u8)k;
                if (g_ins) {
                    undo_push(pos, 0, 0, &c, 1);
                    insert_at(pos, c);
                    g_col++;
                } else {
                    undo_push(pos, &g_buf[pos], 1, &c, 1);
                    replace_at(pos, c);
                    g_col++;
                }
                rebuild_lines();
                refresh();
                continue;
            }
        } else {
            /* 十六进制模式 */
            if (k == KB_UP) { if (g_hpos >= 16) g_hpos -= 16; if (g_hpos < g_hex_top) g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            if (k == KB_DOWN) { if (g_hpos + 16 < g_len) g_hpos += 16; else if (g_hpos + 1 < g_len) g_hpos++; if (g_hpos >= g_hex_top + 352) g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            if (k == KB_LEFT) { if (g_hpos > 0) g_hpos--; if (g_hpos < g_hex_top) g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            if (k == KB_RIGHT) { if (g_hpos + 1 < g_len) g_hpos++; if (g_hpos >= g_hex_top + 352) g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            if (k == KB_PGUP) { g_hpos = g_hpos >= 352 ? g_hpos - 352 : 0; g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            if (k == KB_PGDN) { g_hpos += 352; if (g_hpos >= g_len) g_hpos = g_len ? g_len - 1 : 0; g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            if (k == KB_HOME) { g_hpos = 0; g_hex_top = 0; refresh(); continue; }
            if (k == KB_END) { g_hpos = g_len ? g_len - 1 : 0; g_hex_top = g_hpos - (g_hpos % 16); refresh(); continue; }
            /* hex 输入：两位 hex 写字节 */
            if ((k >= '0' && k <= '9') || (k >= 'a' && k <= 'f') || (k >= 'A' && k <= 'F')) {
                int v = hex_val((char)k);
                u8 old = g_hpos < g_len ? g_buf[g_hpos] : 0;
                if (g_hpos < g_len) {
                    u8 nv = (u8)((old & 0xF0) | (v & 0xF));
                    /* 高位已由上次设置？简化：直接低 4 位替换，配合"按两次"逻辑省略，直接替换低半字节 */
                    /* 更实用：输入替换整个字节需两位。此处实现：第一次输高位存临时 */
                    static int hex_nib = 0;      /* 0=高 1=低 */
                    static u8 hex_high = 0;
                    if (!hex_nib) { hex_high = (u8)(v & 0xF); hex_nib = 1; }
                    else {
                        nv = (u8)((hex_high << 4) | (v & 0xF));
                        undo_push(g_hpos, &old, 1, &nv, 1);
                        replace_at(g_hpos, nv);
                        hex_nib = 0;
                        if (g_hpos + 1 < g_len) g_hpos++;
                        rebuild_lines();
                    }
                } else {
                    /* 追加新字节 */
                    static int hex_nib2 = 0;
                    static u8 hex_high2 = 0;
                    if (!hex_nib2) { hex_high2 = (u8)(v & 0xF); hex_nib2 = 1; }
                    else {
                        u8 nv = (u8)((hex_high2 << 4) | (v & 0xF));
                        undo_push(g_len, 0, 0, &nv, 1);
                        insert_at(g_len, nv);
                        g_hpos = g_len - 1;
                        hex_nib2 = 0;
                        rebuild_lines();
                    }
                }
                refresh();
                continue;
            }
            if (k == KB_BS) {
                /* 删除当前字节 */
                if (g_hpos < g_len) {
                    u8 old = g_buf[g_hpos];
                    undo_push(g_hpos, &old, 1, 0, 0);
                    delete_at(g_hpos);
                    rebuild_lines();
                }
                refresh();
                continue;
            }
            if (k == KB_DEL) {
                if (g_hpos < g_len) {
                    u8 old = g_buf[g_hpos];
                    undo_push(g_hpos, &old, 1, 0, 0);
                    delete_at(g_hpos);
                    rebuild_lines();
                }
                refresh();
                continue;
            }
        }
    }
}
