# vcs.csc - external version control for ot (CSC 2.1)
set vd = .vcs
set empty = 
set cmd = $1
if $cmd == init goto init
if $cmd == usage goto usage
exists $vd/info ie
if $ie == 0 goto notinit
mergeb $vd/info
rows A nh
row A 0 head
mergeb $vd/index
rows A ni
if $cmd == add goto add
if $cmd == commit goto commit
if $cmd == log goto log
if $cmd == status goto status
goto usage
:init
exists $vd/info ie
if $ie == 1 goto end
run mkdir $vd
write $vd/info 0
write $vd/index 
write $vd/log 
goto end
:notinit
echo 12 vcs: not initialized, run vcs init first
goto end
:usage
echo 12 usage: vcs init|add F|commit MSG|log|status
goto end
:add
if $2 == $empty goto usage
exists $2 fe
if $fe == 0 goto end
set i = 0
:adloop
if $i >= $ni goto adnew
row A $i en
tok en 1 nm
same nm 2 already
set i = $i + 1
goto adloop
:already
echo 12 vcs: already tracked: $2
goto end
:adnew
write /tmp/vcsadd $ni $2
mergeb /tmp/vcsadd
saveb $vd/index
echo 13 vcs: tracked: $2
goto end
:commit
set nv = $head + 1
revd $nv rd
run mkdir $rd
set i = 0
:cmloop
if $i >= $ni goto cmdone
row A $i en
tok en 1 fn
exists $fn fe
if $fe == 0 goto cmskip
fsize $fn sz
if $sz > 8192 goto cmskip
load $fn
filen $rd $i fb
saveb $fb
goto cmnext
:cmskip
echo 12 vcs: >8KB or missing, skip: $fn
:cmnext
set i = $i + 1
goto cmloop
:cmdone
clrb
write $vd/info $nv
now t
write /tmp/vcslog $nv|$t|$2
mergeb /tmp/vcslog
mergeb $vd/log
saveb $vd/log
echo 13 vcs: committed rev $nv
goto end
:log
clrb
mergeb $vd/log
rows A nl
set i = 0
:lloop
if $i >= $nl goto end
row A $i l
echo 15 $l
set i = $i + 1
goto lloop
:status
revd $head rd
set i = 0
:sttop
if $i >= $ni goto end
row A $i en
tok en 1 fn
filen $rd $i base
exists $base se
if $se == 0 goto stnew
snapb $base
loadb $fn
cmpb md
if $md == 0 goto stsame
echo 14   [MODIFIED] $fn
goto stnext
:stsame
echo 13   [SAME] $fn
goto stnext
:stnew
echo 12   [NEW] $fn
:stnext
set i = $i + 1
goto sttop
:end
