set -e
mkdir -p out

DEBUG_CFLAGS=""
if [ "${DEBUG:-0}" != "0" ]; then
  DEBUG_CFLAGS="-DDEBUG"
fi

# TAP build: loaded via DIVIDE on real +3
zcc +zx -vn -clib=new ${DEBUG_CFLAGS} -create-app disk_tester.c intstate.asm -o ./out/disk_tester -m

# DSK build: bootable +3 disk image
zcc +zx -vn -clib=new ${DEBUG_CFLAGS} -subtype=plus3 -create-app disk_tester.c intstate.asm -o ./out/disk_tester_plus3 -m

z88dk-dis --target out/disk_tester > out/disk_tester.asm
