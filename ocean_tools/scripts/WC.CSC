# wc.csc - line count for a file (wc -l analog)
# usage: wc FILE
set empty = 
if $1 == $empty goto usage
exists $1 fe
if $fe == 0 goto notfound
mergeb $1
rows A n
echo 15 wc: $n lines
goto end
:usage
echo 12 usage: wc FILE
goto end
:notfound
echo 12 wc: no such file
:end
