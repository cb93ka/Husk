#!/bin/bash
# ci_guest_files.sh -- on a CI runner, the parts of the app that come out of the Lineage guest image:
#
#   build/java-runtime     libcore, ICU, tzdata, conscrypt, Husk's Java framework, framework-res (Java-only apps)
#   build/android-system   fonts and root certificates (Flutter and other apps that read /system directly)
#
# A full local build takes these from an unpacked image on the developer's disk (build_java_runtime.sh's defaults);
# here the image is the one the app itself downloads (release lineage-v2), unpacked on the runner.
#
# Needs: gh (authenticated), qemu-img and debugfs (Homebrew qemu, e2fsprogs), a JDK in $JAVA_HOME.
set -euo pipefail

R="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE_TAG="${IMAGE_TAG:-lineage-v2}"
IMAGE_NAME="${IMAGE_NAME:-vda-v12.qcow2}"
IMAGE_REPO="${IMAGE_REPO:-Leviidev/Husk}"
R8_VERSION="${R8_VERSION:-8.5.35}"
WORK="${WORK:-$(mktemp -d)}"
DEBUGFS="$(command -v debugfs || echo /opt/homebrew/opt/e2fsprogs/sbin/debugfs)"
mkdir -p "$WORK"

echo "==> guest image $IMAGE_NAME"
[ -f "$WORK/$IMAGE_NAME" ] || gh release download "$IMAGE_TAG" -R "$IMAGE_REPO" -p "$IMAGE_NAME" -D "$WORK"
"${QEMU_IMG:-qemu-img}" convert -O raw "$WORK/$IMAGE_NAME" "$WORK/vda.raw"
rm -f "$WORK/$IMAGE_NAME"

echo "==> fonts and certificates"
"$R/scripts/build_android_system.sh" "$WORK/vda.raw"

echo "==> system partition"
# The same unpacking of the super partition build_android_system.sh does, kept for the APEXes and framework files below.
python3 - "$WORK/vda.raw" "$WORK" <<'EOF'
import struct, subprocess, sys
raw, work = sys.argv[1], sys.argv[2]
out = subprocess.run(['gpt', '-r', 'show', raw], capture_output=True, text=True).stdout
start = [int(l.split()[0]) for l in out.splitlines() if l.split()[2:4] == ['2', 'GPT']][0]
base = start * 512
f = open(raw, 'rb')
f.seek(base + 12288); h = f.read(256)
magic, _, _, hsize = struct.unpack_from('<IHHI', h, 0)
assert magic == 0x414C5030, 'no logical partition metadata'
o = 12 + 32; tsize = struct.unpack_from('<I', h, o)[0]; o += 4 + 32
descs = [struct.unpack_from('<III', h, o + 12 * i) for i in range(4)]
f.seek(base + 12288 + hsize); T = f.read(tsize)
(po, pn, ps), (eo, en, es) = descs[0], descs[1]
ext = [struct.unpack_from('<QIQI', T, eo + es * i) for i in range(en)]
for i in range(pn):
    e = T[po + ps * i:po + ps * (i + 1)]
    name = e[:36].split(b'\0')[0].decode(); _, fi, ne, _ = struct.unpack_from('<IIII', e, 36)
    if name != 'system': continue
    with open(work + '/system.img', 'wb') as w:
        for j in range(fi, fi + ne):
            nsec, _, data, _ = ext[j]
            f.seek(base + data * 512); left = nsec * 512
            while left:
                b = f.read(min(left, 64 << 20)); w.write(b); left -= len(b)
EOF
rm -f "$WORK/vda.raw"
SYS="$WORK/system.img"

echo "==> APEXes"
APEX="$WORK/apex"; mkdir -p "$APEX"
LIST="$("$DEBUGFS" -R "ls -p /system/apex" "$SYS" 2>/dev/null | awk -F/ '{print $6}')"
for mod in com.android.art com.android.i18n com.android.tzdata com.android.conscrypt; do
    file="$(echo "$LIST" | grep -E "^${mod//./\\.}(\.[a-z]+)?\.c?apex$" | head -1)"
    [ -n "$file" ] || { echo "no $mod in /system/apex:"; echo "$LIST"; exit 1; }
    "$DEBUGFS" -R "dump /system/apex/$file $WORK/$file" "$SYS" 2>/dev/null
    python3 - "$WORK/$file" "$WORK/$mod.img" <<'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
if 'original_apex' in z.namelist(): z = zipfile.ZipFile(z.open('original_apex'))
open(sys.argv[2], 'wb').write(z.read('apex_payload.img'))
EOF
    mkdir -p "$APEX/$mod"
    for d in javalib lib64 etc; do "$DEBUGFS" -R "rdump /$d $APEX/$mod" "$WORK/$mod.img" 2>/dev/null || true; done
    rm -f "$WORK/$file" "$WORK/$mod.img"
    echo "    $mod <- $file: $(du -sh "$APEX/$mod" | cut -f1)"
done

echo "==> framework files"
"$DEBUGFS" -R "dump /system/framework/org.apache.http.legacy.jar $WORK/org.apache.http.legacy.jar" "$SYS" 2>/dev/null
"$DEBUGFS" -R "dump /system/framework/framework-res.apk $WORK/framework-res.apk" "$SYS" 2>/dev/null
rm -f "$SYS"

echo "==> R8 $R8_VERSION"
curl -fsSL -o "$WORK/r8.jar" "https://dl.google.com/android/maven2/com/android/tools/r8/$R8_VERSION/r8-$R8_VERSION.jar"

echo "==> Java runtime"
HUSK_JDK="$JAVA_HOME" HUSK_D8="$WORK/r8.jar" HTTP_LEGACY="$WORK/org.apache.http.legacy.jar" FRAMEWORK_RES="$WORK/framework-res.apk" \
    "$R/scripts/build_java_runtime.sh" "$APEX"
