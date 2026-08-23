# Sourced by every runner here that builds with -fsanitize=address,undefined.
# Not executable on its own.
#
# A sanitizer report does not fail these suites by itself: UBSan prints
# "runtime error: ..." and carries on, and ASan's default exit code is 1, which
# is a WANTED exit code in run.sh. Both settings below turn the first report
# into SIGABRT (exit 134), which no case wants.
#
# detect_leaks=0 on purpose: these CLIs are short-lived by design.
export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export UBSAN_OPTIONS="abort_on_error=1:halt_on_error=1:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

# Second line of defence for a runner that captures output and compares exit
# codes by hand: true when <text> carries a sanitizer report.
san_report() {
  case "$1" in
    *Sanitizer*|*"runtime error:"*) return 0 ;;
  esac
  return 1
}
