# Build and test image of the Linux Legacy UI on the lowest testable Linux system: the
# manylinux2010 image of PyPA (CentOS 6.10, glibc 2.12, devtoolset-8 gcc 8.3.1, cmake 3.23.3,
# Python 3.6 to 3.10 under /opt/python; the system Python is 2.6).
# CentOS 6 is end of life: the yum repositories of that image already point at vault.centos.org,
# which answers "403 Forbidden" to the CentOS 6 TLS stack for the repodata files (seen on
# 2026-11-15; it had answered the day's first build). WORKAROUND: the base, updates and extras
# repositories are pointed at the same vault content on archive.kernel.org/centos-vault/6.10/.
# The SCLo and EPEL repositories (same host, same 403) are not used. Added: the Xft and fontconfig development files (the image has libX11-devel only), a virtual
# X server and fonts for the test run. CMake and the compiler are the image's own; no other tool is
# installed. The build uses Unix Makefiles (no Ninja).
# Build: docker build -t loomora-manylinux2010 tools/trials/linux-manylinux2010-x86_64
FROM quay.io/pypa/manylinux2010_x86_64
RUN sed -i -e 's#https://vault.centos.org/centos/$releasever/#https://archive.kernel.org/centos-vault/6.10/#' \
        /etc/yum.repos.d/CentOS-Base.repo \
    && yum --disablerepo='*' --enablerepo=base,updates,extras install -y libXft-devel fontconfig-devel xorg-x11-server-Xvfb xorg-x11-fonts-misc \
        xorg-x11-fonts-Type1 dejavu-sans-fonts dejavu-sans-mono-fonts dejavu-serif-fonts \
    && yum clean all
