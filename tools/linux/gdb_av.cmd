handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIG34 nostop noprint pass
handle SIG35 nostop noprint pass
handle SIG36 nostop noprint pass
set pagination off
set breakpoint pending on
catch load librexruntime
run
break _ZN3rex6memory6Memory23AccessViolationCallbackESt11unique_lockISt15recursive_mutexEPvb if $rdx == 0x1706A0008
info breakpoints
continue
bt 30
kill
