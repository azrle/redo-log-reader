# Innodb redo log reader
This is a dirty work. It only works for some mtr types now.

## Goal
It could be used for learning Innodb redo log structure.

## How to use
```
gcc -x c -std=gnu89 -O2 -Wall redo_log_reader.cc -o bin/rlr
RLR_DBG=1 bin/rlr test/ib_logfile0 | less
```
