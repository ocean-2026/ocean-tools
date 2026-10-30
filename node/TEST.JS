/* CatOS node-subset acceptance script (runs from disk: node /BIN/TEST.JS) */
function add(a, b) { return a + b; }
function greet(who) { return "hello " + who; }
function fib(n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
var v = add(20, 22);
var a = [5, 6, 7];
a.push(8);
a[0] = 1;
var o = { name: "catos", ver: 2, meta: { arch: "i386" }, nums: a };
o.level = 3;
var i = 0;
var sum = 0;
while (i < a.length) { sum = sum + a[i]; i = i + 1; }
var s = "";
var j = 0;
while (j < 4) { if (j == 2) { j = j + 1; continue; } s = s + j; j = j + 1; }
print("v=", v);
print("greet=", greet("world"));
print("fib(10)=", fib(10));
print("sum=", sum);
print("s=", s);
print("o.name=", o.name, "ver=", o.ver, "level=", o.level);
print("o.nums[2]=", o.nums[2], "meta.arch=", o.meta.arch);
print("typeof v=", typeof v, "arr=", typeof a, "obj=", typeof o);
if (v == 42 && sum == 22 && s == "013" && fib(10) == 55) print("ALL PASS");
else print("FAIL");
