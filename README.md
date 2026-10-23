# Ocean's Tools (ot / oceanshell / osh)

CatOS i386 userland full-screen command line (CE app) for CatOS on QEMU/Bochs.

## Features

- Full-screen character terminal shell with pixel banner **OCEAN'S TOOLS**
- Config file /BIN/OCNTOOLS.CFG: title/tagline/welcome/startup commands
- 16-entry history, Tab completion, alias, multicolor prompt
- **CSC script language (CSC 2.1)**: 32 vars, 4x64 arrays, 8K file buffers fa/fb, for stack 4, positional params $0-$9, string funcs, file cmds (load/loadb/rows/row/write/saveb/mergeb/dirb/clrb), system primitives (key/peek/poke/alloc/free/arg/exists/fsize/now/pad2/revd/filen/snapb/cmpb), integer arithmetic
- **External command mechanism**: /bin/*.csc auto-registered (v1.6: diff/vk; v1.7: sft/vcs external; CSC interpreter is the only mandatory builtin)
- Builtin tools: edt(hexedit), fm(fmgr), cedbg + cat/ls/rm/touch/mkdir/cd/pwd etc

## Versions

- v1.7: CSC 2.1 system primitives (key/peek/poke/alloc etc) + sft/vcs externalized (image 2.8KB smaller than v1.6)
- v1.6: CSC 2.0 syntax core + external diff/vk
- v1.4: seven catos-ce-tools integrated
- v1.1~v1.5: config system, echo 16 colors, history/Tab/alias

## Build

```sh
bash build.sh   # -Os compile -> ocntools.ce (<=64KiB, CE01 header)
```

Inject into 64MiB FAT32 image /BIN/OCNTOOLS.CE via python3 fatfs.py add_file_safe.

## Layout

- ocean_tools.c main source; hexedit_port.c / fmgr_port.c ported tools; scripts/ external CSC scripts (sft.csc 108 lines / vcs.csc 121 lines); build.sh; OCNTOOLS.CFG; image /BIN holds diff.csc vk.csc sft.csc vcs.csc + ocntools.v15.ce/ocntools.v16.ce backups
