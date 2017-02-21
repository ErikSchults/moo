#ifndef H_SYSCALLS
#define H_SYSCALLS

#define SYSCALL_EXIT 1
#define SYSCALL_FORK 2
#define SYSCALL_READ 3
#define SYSCALL_WRITE 4
#define SYSCALL_OPEN 5
#define SYSCALL_CLOSE 6
#define SYSCALL_BRK 8
#define SYSCALL_STAT 9
#define SYSCALL_GETPID 10
#define SYSCALL_EXECVE 11
#define SYSCALL_CHDIR 12
#define SYSCALL_GETGID 13
#define SYSCALL_TCSETPGRP 14
#define SYSCALL_LSEEK 15
#define SYSCALL_TCGETPGRP 16
#define SYSCALL_SETPGRP 17
#define SYSCALL_FCNTL 18
#define SYSCALL_GETPGRP 19
#define SYSCALL_SIGACTION 20
#define SYSCALL_MKDIR 21
#define SYSCALL_WAITPID 22
#define SYSCALL_SIGSUSPEND 23
#define SYSCALL_DEBUG 24
#define SYSCALL_GETPPID 25
#define SYSCALL_GETEGID 26
#define SYSCALL_GETEUID 27
#define SYSCALL_GETUID 28

#endif
