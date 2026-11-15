# Test-environment patch for the manylinux1 container (CentOS 5.11, glibc 2.5); see the Dockerfile.
# Written for Python 2.4, the system Python of CentOS 5 (no "with", no conditional expression).
#
# glibc 2.5 implements time() and gettimeofday() as a call into the fixed vsyscall page at
# 0xffffffffff600400 and 0xffffffffff600000. The Docker Desktop (WSL2) kernel does not map that
# page (vsyscall=none), so the first call kills the process with SIGSEGV: bash, perl, python3,
# yum, the compiler proper (cc1) and getaddrinfo all die. An LD_PRELOAD library cannot fix this
# (libc calls its own functions without the PLT), so the two call sites of libc-2.5.so are
# rewritten to issue the real system calls, which every kernel has:
#   mov $0xffffffffff600400,%rax ; call *%rax   ->   mov $201,%eax ; syscall ; nop ; nop   (time)
#   mov $0xffffffffff600000,%rax ; call *%rax   ->   mov $96,%eax  ; syscall ; nop ; nop   (gettimeofday)
# A real CentOS 5 machine does not need this. The new file replaces the old one by rename, so
# running processes keep the old inode.
import os
import sys

PATH = "/lib64/libc-2.5.so"
PATCHES = [
    ("time", "\x48\xc7\xc0\x00\x04\x60\xff\xff\xd0", "\xb8\xc9\x00\x00\x00\x0f\x05\x90\x90"),
    ("gettimeofday", "\x48\xc7\xc0\x00\x00\x60\xff\xff\xd0", "\xb8\x60\x00\x00\x00\x0f\x05\x90\x90"),
]

f = open(PATH, "rb")
try:
    data = f.read()
finally:
    f.close()
for name, old, new in PATCHES:
    if data.count(old) != 1:
        sys.stderr.write("%s: %d occurrences of the %s call site, expected 1\n"
                         % (PATH, data.count(old), name))
        sys.exit(1)
    data = data.replace(old, new)
    sys.stdout.write("patched the %s call site\n" % name)
tmp = PATH + ".new"
f = open(tmp, "wb")
try:
    f.write(data)
finally:
    f.close()
os.chmod(tmp, 0755)
os.rename(tmp, PATH)
