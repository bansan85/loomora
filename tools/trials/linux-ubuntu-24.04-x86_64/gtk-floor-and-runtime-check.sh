#!/bin/sh
# Two measurements behind docs/adr/0004-fallback-per-case.md, for the GTK Modern UI on Linux.
# Run inside WSL from the repository root (the build goes under $HOME, never /tmp):
#   wsl -d Ubuntu-24.04 --cd <repo> -- sh validation/evidence/linux-ubuntu-24.04-x86_64/adr-0004/gtk-floor-and-runtime-check.sh
#
# 1. Does the GTK 3.10 floor (GDK_VERSION_MIN_REQUIRED / MAX_ALLOWED) change the code that is
#    generated? main.c and probe.c are compiled with the flags of meson.build and without them,
#    with the same other flags, and the object files are compared byte for byte.
# 2. What does the choice of the C++ runtime cost? The same objects (the backend compiled as
#    libloomora.a does, with the flags of meson.build) are linked with the system libstdc++ and
#    with -static-libstdc++ -static-libgcc; size, DT_NEEDED entries and the highest symbol versions
#    of GLIBC / GLIBCXX the executable asks for are printed.
set -e
R=$(pwd)
G=$R/ui/modern/linux-gtk3
C=$R/core
B=$HOME/loomora-build/adr0004-gtk
rm -rf "$B"
mkdir -p "$B"
cd "$B"
CF=$(pkg-config --cflags gtk+-3.0)
FLOOR="-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_10 -DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_3_10"

echo "== GTK $(pkg-config --modversion gtk+-3.0), $(gcc --version | head -1)"
echo "== 1. object code with and without the GTK 3.10 floor (gcc -std=c99 -O3 -Wall -Wextra)"
for f in main probe; do
  gcc -std=c99 -O3 -Wall -Wextra $CF -I"$C/include" $FLOOR -c "$G/$f.c" -o floor_$f.o
  gcc -std=c99 -O3 -Wall -Wextra $CF -I"$C/include" -c "$G/$f.c" -o nofloor_$f.o
  if cmp -s floor_$f.o nofloor_$f.o; then echo "$f.o: identical with and without the floor"; else echo "$f.o: DIFFERENT"; fi
done
echo "GTK / GDK functions the UI calls:"
nm -u floor_main.o floor_probe.o | awk '/ U (gtk|gdk)_/ {print $2}' | sort -u | tr '\n' ' '
echo

echo
echo "== 2. C++ runtime: system libstdc++ against -static-libstdc++ -static-libgcc"
for f in c_api eol greet probe utf8 version; do
  g++ -std=c++11 -O3 -pedantic -fvisibility=hidden -fvisibility-inlines-hidden -I"$C/include" -c "$C/src/$f.cpp" -o core_$f.o
done
LIBS=$(pkg-config --libs gtk+-3.0)
g++ -O3 floor_main.o floor_probe.o core_*.o $LIBS -o gtk_dynamic
g++ -O3 floor_main.o floor_probe.o core_*.o $LIBS -static-libstdc++ -static-libgcc -o gtk_static
ls -l gtk_dynamic gtk_static | awk '{print $5, $9}'
for e in gtk_dynamic gtk_static; do
  echo "-- $e"
  echo "DT_NEEDED: $(readelf -d $e | awk '/NEEDED/ {gsub(/[\[\]]/, "", $5); printf "%s ", $5}')"
  for k in GLIBCXX CXXABI GLIBC; do
    v=$(objdump -T $e | grep -o "${k}_[0-9.]*" | sort -uV | tail -1)
    echo "highest ${k}: ${v:-none}"
  done
done
