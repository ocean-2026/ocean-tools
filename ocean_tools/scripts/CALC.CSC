# calc.csc - base calculator (CSC wrapper around ot eval)
# usage: calc EXPR  e.g. calc 2*3+4  calc (6+2)/4
set empty = 
if $1 == $empty goto usage
run eval $1
goto end
:usage
echo 12 usage: calc EXPR  (integer + - * / with parens)
:end
