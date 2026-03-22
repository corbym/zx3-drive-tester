set -e

rm -rf out
mkdir -p out

DEBUG_CFLAGS=""
if [ "${DEBUG:-0}" != "0" ]; then
  DEBUG_CFLAGS="-DDEBUG"
fi

UI_CFLAGS=""
if [ "${COMPACT_UI:-0}" != "0" ]; then
  UI_CFLAGS="-DCOMPACT_UI=1"
fi

HEADLESS_FONT_CFLAGS=""
if [ "${HEADLESS_ROM_FONT:-0}" != "0" ]; then
  HEADLESS_FONT_CFLAGS="-DHEADLESS_ROM_FONT=1"
fi

# Keep only printf handlers used by this project (%u, %X, %s) plus flag parsing
# for width/zero-pad forms like %3u and %02X.
PRINTF_CFLAGS="-pragma-define:CLIB_OPT_PRINTF=0x4000A20A -pragma-define:CLIB_OPT_PRINTF_2=0"
OPT_CFLAGS="-SO3"

# TAP build: loaded via DIVIDE on real +3
zcc +zx -vn -clib=new ${OPT_CFLAGS} ${DEBUG_CFLAGS} ${UI_CFLAGS} ${HEADLESS_FONT_CFLAGS} ${PRINTF_CFLAGS} -create-app disk_tester.c disk_operations.c menu_system.c ui.c test_cards.c intstate.asm -o ./out/disk_tester -m

# DSK build: bootable +3 disk image
zcc +zx -vn -clib=new ${OPT_CFLAGS} ${DEBUG_CFLAGS} ${UI_CFLAGS} ${HEADLESS_FONT_CFLAGS} ${PRINTF_CFLAGS} -subtype=plus3 -create-app disk_tester.c disk_operations.c menu_system.c ui.c test_cards.c intstate.asm -o ./out/disk_tester_plus3 -m

z88dk-dis out/disk_tester_CODE.bin > out/disk_tester.asm || true
