// MINI_GO.GO -- a mini Go-subset interpreter written IN the go subset itself.
//
// This is the self-hosting proof: it runs under go.ce and interprets a
// smaller Go-subset program (argv[2], e.g. /BIN/MINI_IN.GO).  The tiny target
// language supports only:
//     x = <number>
//     x = y <op> z        (op one of + - *)
//     print x
// with 26 single-letter registers a..z.
//
// On CatOS:   run /bin/go.ce /BIN/MINI_GO.GO /BIN/MINI_IN.GO
// Expected output for MINI_IN.GO:   7

var src = readfile(argv(2))
var reg = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
var toks = []
var cur = ""
var i = 0

// --- tokenize src on whitespace ---
for i < len(src) {
    var c = src[i]
    if c == ' ' || c == '\n' || c == '\r' || c == '\t' {
        if len(cur) > 0 {
            toks = append(toks, cur)
            cur = ""
        }
    } else {
        cur = cur + chr(c)
    }
    i = i + 1
}
if len(cur) > 0 {
    toks = append(toks, cur)
}

// --- value of a token: number literal or a register ---
func val(t string) int {
    var d = t[0]
    if d >= '0' && d <= '9' {
        return atoi(t)
    }
    return reg[d - 'a']
}

// --- interpret the token stream ---
i = 0
for i < len(toks) {
    var w = toks[i]
    if w == "print" {
        i = i + 1
        var nm = toks[i]
        print(reg[nm[0] - 'a'])
        i = i + 1
    } else {
        var slot = w[0] - 'a'
        i = i + 2              // skip name and '='
        var t1 = toks[i]
        if i + 1 < len(toks) {
            var nxt = toks[i + 1]
            if nxt == "+" || nxt == "-" || nxt == "*" {
                var v1 = val(t1)
                var v2 = val(toks[i + 2])
                var r = v1
                if nxt == "+" { r = v1 + v2 }
                if nxt == "-" { r = v1 - v2 }
                if nxt == "*" { r = v1 * v2 }
                reg[slot] = r
                i = i + 3
            } else {
                reg[slot] = val(t1)
                i = i + 1
            }
        } else {
            reg[slot] = val(t1)
            i = i + 1
        }
    }
}
