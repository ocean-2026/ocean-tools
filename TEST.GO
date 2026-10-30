// TEST.GO -- go subset acceptance (self-checking).
// Prints one PASS line per case and a final ALL PASS.
var fails = 0

func check(name string, got int, want int) {
    if got == want {
        print("PASS", name)
    } else {
        print("FAIL", name, "got=", got, "want=", want)
        fails = fails + 1
    }
}

func fact(n int) int {
    if n <= 1 { return 1 }
    return n * fact(n - 1)
}

func fib(n int) int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}

// arithmetic
check("arith-mul", 2 + 3 * 4, 14)
check("arith-sub", 10 - 3, 7)
check("arith-div", 20 / 6, 3)
check("arith-mod", 20 % 6, 2)
check("arith-neg", -5 + 2, -3)
check("arith-prec", (2 + 3) * 4, 20)

// comparison (bool -> int: true=1)
check("cmp-eq", 3 == 3, 1)
check("cmp-ne", 3 != 4, 1)
check("cmp-lt", 2 < 3, 1)
check("cmp-le", 3 <= 3, 1)
check("cmp-gt", 5 > 5, 0)
check("cmp-ge", 5 >= 5, 1)

// logical
check("log-and", 1 && 0, 0)
check("log-or", 0 || 1, 1)
check("log-not", !0, 1)
check("log-nottrue", !1, 0)

// if / else
var br = 0
if 5 > 3 { br = 1 } else { br = 0 }
check("if-true", br, 1)
if 5 < 3 { br = 1 } else { br = 2 }
check("else-branch", br, 2)

// for loop (three-clause)
var s = 0
for i := 1; i <= 5; i = i + 1 { s = s + i }
check("for-sum", s, 15)

// for loop (while style)
var j = 0
var twos = 0
for j < 5 { twos = twos + j; j = j + 1 }
check("while-sum", twos, 10)

// break / continue
var bsum = 0
for k := 0; k < 10; k = k + 1 {
    if k == 5 { break }
    if k == 2 { continue }
    bsum = bsum + k
}
check("break-continue", bsum, 0 + 1 + 3 + 4)

// recursion
check("fact-5", fact(5), 120)
check("fib-10", fib(10), 55)
check("fact-1", fact(1), 1)

// arrays
var a = [10, 20, 30, 40]
a[1] = 99
check("arr-len", len(a), 4)
check("arr-write", a[1], 99)
check("arr-read", a[0], 10)
var asum = 0
for m := 0; m < len(a); m = m + 1 { asum = asum + a[m] }
check("arr-sum", asum, 10 + 99 + 30 + 40)

// strings
var s1 = "ab" + "cd"
check("str-len", len(s1), 4)
check("str-sub", len(sub(s1, 1, 2)), 2)
check("str-at", s1[0], 97) // 'a'
check("atoi", atoi("42"), 42)
check("chr", chr(65)[0], 65) // 'A' byte

// string compare via equality of lengths + content (use atoi on digits)
var nums = "12" + "34"
check("str-concat-atoi", atoi(nums), 1234)

if fails == 0 {
    print("ALL PASS")
} else {
    print("SOME FAIL fails=", fails)
}
