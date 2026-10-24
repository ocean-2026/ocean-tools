# l3test.csc - L3 func/call/ret smoke test (v1.10)
# func NAME ... ret [EXPR] / call NAME A B C  (literal args in v1.10;
#  variable expansion in call args is a v1.11 candidate - image budget)
echo 14 ==== L3 func/call/ret test ====
func add
echo 15 in add: $1 + $2
ret $3
func double
call add 7 0 99
set r2 = $ret
ret $r2
:main
call add 2 3 42
echo 15 after call: ret=$ret
call double 0 0 0
echo 15 nested-double: ret=$ret
call add 10 20
echo 15 no-3rd-arg: ret=$ret
echo 14 ==== L3 done ====
