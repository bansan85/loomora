set -u
gcc -O2 -o /tmp/xlib-driver /src/tools/trials/linux-ubuntu-24.04-x86_64/xlib-driver.c -lX11 2>&1 | head -3
Xvfb :98 -screen 0 1280x1024x24 -nolisten tcp 2>/tmp/xvfb.err &
sleep 2
export DISPLAY=:98 LANG=C.UTF-8
EXE=/root/loomora-build/linux-legacy-x86_64/loomora-xlib
for lang in C.UTF-8 en_US.UTF-8; do
  echo "== LANG=$lang"; export LANG=$lang
  $EXE 2>/tmp/ui.err & PID=$!
  sleep 1
  /tmp/xlib-driver shot /tmp/a.ppm wait 200 type " Marie" wait 300 shot /tmp/b.ppm 2>&1 | tail -6
  md5sum /tmp/a.ppm /tmp/b.ppm | awk '{print $1}' | uniq | wc -l | sed 's/^/distinct screenshots (2 = typing changed the window): /'
  /tmp/xlib-driver close 2>&1 | tail -2
  wait $PID; echo "ui exit status: $?"; echo "ui stderr bytes: $(wc -c < /tmp/ui.err)"
done
locale -a | head -5
