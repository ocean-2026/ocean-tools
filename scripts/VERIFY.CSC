# verify.csc - v1.9 smoke test (auto-run via CFG STARTUP_CMD, keyboard-agnostic)
echo 14 ========== ot v1.9 smoke ==========
echo 15 --- eval (L1 expr) ---
run eval 2*3+4
run eval (6+2)/4
echo 15 --- swap layer ---
run swapout /BIN/CALC.CSC
run swapinfo
run swapin
echo 15 --- acl ---
run whoami
run acl
echo 15 --- clipboard ---
run clp v1.9-clip
run clp
echo 15 --- allocator ---
run mem info
run mem alloc 32
run mem info
echo 15 --- netdiag ---
run netdiag
echo 15 --- namespaces ---
run net.ping
run fs.size /BIN/CALC.CSC
echo 15 --- ext scripts ---
run calc 0x10+2
run wc /BIN/WC.CSC
run time
echo 14 ========== smoke done ==========
