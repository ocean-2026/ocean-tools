# DEMO.CSC - Ocean's Command Script of CatOS
# variables, arithmetic, if/goto loop, styled echo, run builtin
set name = Ocean
set i = 1
echo 11 Hello from $name's Tools!
:top
echo 14 count = $i
if $i == 3 goto done
set i = $i + 1
goto top
:done
echo 10 loop finished after $i runs
run uname
echo 9  script end, welcome to CSC.
