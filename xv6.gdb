# XV6 Debugging ONLY
# Usage: gdb-multiarch -q -nx -x xv6.gdb

# Initiation and symbols
set confirm off
set architecture riscv:rv64
symbol-file kernel/kernel
target remote 127.0.0.1:26000

set disassemble-next-line auto
set riscv use-compressed-breakpoints yes

set pagination off
set verbose off
set breakpoint pending on

# Pretty print
set print pretty on
set print elements 0
set print repeats 0
set print asm-demangle on

# Source lookup (CHANGE THIS LINE FOR YOUR OWN PROJECT)
directory /home/markus/dev/xv6

# Layout and QoL improvements
tui enable
layout split
focus cmd

# Project history (CHANGE TO YOUR OWN HISTORY BACKUP DIRECTORY)
set history save on
set history filename ~/dev/.gdb_history_xv6
set history size 10000

# Remote QoL
set remotetimeout 10

# Awating for user to set breakpoints