// CatOS go-subset demo: recursion / loops / arrays / strings
func fib(n int) int {
    if n < 2 { return n }
    return fib(n - 1) + fib(n - 2)
}
func add(a int, b int) int { return a + b }
var x = 5
var y = x * 2 + 1
var s = "hello " + "go"
var a = [1, 2, 3, 4]
var total = 0
for i := 0; i < len(a); i = i + 1 { total = total + a[i] }
var evens = 0
for j := 0; j < 6; j = j + 1 {
    if j % 2 == 0 { evens = evens + j }
}
print("x=", x, " y=", y, " s=", s)
print("fib(10)=", fib(10))
print("add(3,4)=", add(3, 4))
print("total=", total, " evens=", evens, " a[2]=", a[2])
print("demo done")
