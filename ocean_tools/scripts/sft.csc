# sft.csc - external sift/grep for ot v1.7 (CSC 2.1)
# usage: sft PATTERN FILE... [-i] [-c] [-r]
set empty = 
set ic = 0
set cc = 0
set rr = 0
set nf = 0
set pat = $1
if $pat == $empty goto usage
set i = 2
:argloop
if $i > 8 goto scan
arg $i a
if $a == -i goto opti
if $a == -c goto optc
if $a == -r goto optr
set files[$nf] = $a
set nf = $nf + 1
goto argnext
:opti
set ic = 1
goto argnext
:optc
set cc = 1
goto argnext
:optr
set rr = 1
:argnext
set i = $i + 1
goto argloop
:usage
echo 12 usage: sft PATTERN FILE... [-i] [-c] [-r]
goto end
:scan
if $ic == 1 goto prep
set sp = $pat
goto scanfiles
:prep
upper 1 sp
:scanfiles
set total = 0
set fi = 0
:fileloop
if $fi >= $nf goto summary
set f = $files[$fi]
set cur = $f
if $rr == 0 goto doscan
dirb $f
rows A nd
if $nd > 0 goto dirscan
:doscan
load $cur
rows A nl
set fh = 0
set li = 0
:loop2
if $li >= $nl goto fsum
row A $li l
if $ic == 0 goto match2
upper l lu
set l2 = $lu
goto chk2
:match2
set l2 = $l
:chk2
find l2 sp r
if $r < 0 goto nline2
set fh = $fh + 1
set total = $total + 1
if $cc == 1 goto nline2
echo 14 $cur:$li: $l
:nline2
set li = $li + 1
goto loop2
:fsum
if $cc == 0 goto nextfile
if $fh > 0 goto fcnt
goto nextfile
:fcnt
echo 14 $cur: $fh line(s)
:nextfile
if $rr == 1 goto endscan_dir
set fi = $fi + 1
goto fileloop
:endscan_dir
set di = $di + 1
goto dirloop
:dirscan
set di = 0
:dirloop
if $di >= $nd goto dfi
row A $di ent
tok ent 0 ty
if $ty == D goto nextent
tok ent 1 nm
set cur = $f/$nm
goto doscan
:nextent
set di = $di + 1
goto dirloop
:dfi
set fi = $fi + 1
goto fileloop
:summary
if $cc == 0 goto end
echo 14 sft: $total hit line(s)
:end
echo 13 sft done
