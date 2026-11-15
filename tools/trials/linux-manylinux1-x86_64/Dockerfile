# Build and test image of the Linux Legacy UI on an older Linux userland than manylinux2010: the
# manylinux1 image of PyPA (CentOS 5.11, glibc 2.5, devtoolset-2 gcc 4.8.2, Python 3.5 to 3.9
# under /opt/python; the system Python is 2.4; no CMake).
# 1. THE HOST KERNEL MUST MAP THE VSYSCALL PAGE. glibc 2.5 reads the time through the fixed address
#    0xffffffffff600000, which a kernel started with vsyscall=none (the default of the WSL2 kernel
#    of Docker Desktop) does not map: bash, perl, python3, yum, cc1 and getaddrinfo then die with
#    SIGSEGV (exit status 139) as soon as the image runs. The image itself is NOT modified: the
#    host kernel runs with vsyscall=emulate (kernelCommandLine in .wslconfig, then wsl --shutdown),
#    as every kernel of the CentOS 5 era does. Check: grep vsyscall /proc/cmdline.
# 2. The yum repositories of the image already point at http://archive.kernel.org/centos-vault/5.11/
#    (the CentOS 5 TLS stack is far too old for https, so it is http only); the EPEL and CERN
#    repositories are dead and are not used. The image has a centosplus build of libselinux, so
#    that repository (libselinux) is enabled too and the 32-bit packages are excluded, or yum
#    cannot resolve its dependencies. Added: the Xft and fontconfig development files, a
#    virtual X server and fonts for the test run.
# 3. Xvfb 1.1.1 needs rgb.txt (xorg-x11-server-utils; without it no colour name resolves, Xft finds no
#    colour and the UI silently falls back to the core font) and, started without a font path, waits
#    2 to 5 seconds for the font server unix/:7100 that is not there: /usr/local/bin/Xvfb is a
#    wrapper that adds -fp with the misc and Type1 font directories.
# 4. CMake: the image has none and the project needs 3.13. The pip wheel of CMake 3.22.x
#    (manylinux1, built for old glibc) is installed into the Python 3.8 of the image (no CMake
#    from CentOS 5). The build uses Unix Makefiles (no Ninja).
# Build: docker build -t loomora-manylinux1 tools/trials/linux-manylinux1-x86_64
FROM quay.io/pypa/manylinux1_x86_64
COPY Xvfb /usr/local/bin/Xvfb
RUN yum --disablerepo='*' --enablerepo=base,updates,extras,libselinux --exclude='*.i386' --exclude='*.i686' install -y libXft-devel fontconfig-devel xorg-x11-server-Xvfb xorg-x11-server-utils xorg-x11-fonts-misc \
        xorg-x11-fonts-Type1 dejavu-lgc-fonts bitstream-vera-fonts \
    && yum clean all
RUN chmod 755 /usr/local/bin/Xvfb
RUN /opt/python/cp38-cp38/bin/python3 -m pip install "cmake<3.23" \
    && ln -s /opt/python/cp38-cp38/bin/cmake /usr/local/bin/cmake && ln -s /opt/python/cp38-cp38/bin/ctest /usr/local/bin/ctest
