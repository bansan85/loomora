/* Test shim of the Linux Modern trial: makes uname(2) report kernel 2.6.32 so that the
   end-of-life warning of the application is displayed. A SIMULATION: the kernel is not old,
   only the string the application reads is changed.
   Build (inside WSL, in a scratch directory under $HOME):
     gcc -shared -fPIC -o fake-uname.so fake-uname.c -ldl
   Use:
     LD_PRELOAD=$PWD/fake-uname.so ./loomora-gtk3 --probe     (probe line: eol_warning_shown true) */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <sys/utsname.h>

int uname(struct utsname *buf) {
    int (*real)(struct utsname *) = (int (*)(struct utsname *))dlsym(RTLD_NEXT, "uname");
    int r = real(buf);
    if (r == 0) {
        strncpy(buf->release, "2.6.32-fake-uname-shim", sizeof(buf->release) - 1);
        buf->release[sizeof(buf->release) - 1] = '\0';
    }
    return r;
}
