# verify.csc - v1.10 smoke test (auto-run via CFG STARTUP_CMD, keyboard-agnostic)
echo 8 ========== ot v1.10 smoke ==========
echo 8 --- eval (L1 expr) ---
run eval 2*3+4
run eval (6+2)/4
echo 8 --- swap layer ---
run swapout /BIN/CALC.CSC
run swapinfo
run swapin
echo 8 --- acl ---
run whoami
run acl
echo 8 --- clipboard ---
run clp v1.10-clip
run clp
echo 8 --- allocator ---
run mem info
run mem alloc 32
run mem info
echo 8 --- netdiag ---
run netdiag
echo 8 --- namespaces ---
run net.ping
run fs.size /BIN/CALC.CSC
echo 8 --- ext scripts ---
run calc 0x10+2
run wc /BIN/WC.CSC
run time
echo 14 ========== smoke done ==========
