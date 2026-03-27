set -e

OUT_DIR="${OUT_DIR:-out}"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

DEBUG_CFLAGS=""
if [ "${DEBUG:-0}" != "0" ]; then
  DEBUG_CFLAGS="-DDEBUG"
fi

HEADLESS_FONT_CFLAGS=""
if [ "${HEADLESS_ROM_FONT:-0}" != "0" ]; then
  HEADLESS_FONT_CFLAGS="-DHEADLESS_ROM_FONT=1"
fi

# Keep only printf handlers used by this project (%u, %X, %s) plus flag parsing
# for width/zero-pad forms like %3u and %02X.
PRINTF_CFLAGS="-pragma-define:CLIB_OPT_PRINTF=0x4000A20A -pragma-define:CLIB_OPT_PRINTF_2=0"
# +3 build is RAM-tight; reclaim default stdio heap reserve.
HEAP_CFLAGS="-pragma-define:CLIB_STDIO_HEAP_SIZE=0"
OPT_CFLAGS="-SO3"

# TAP build: loaded via DIVIDE on real +3
zcc +zx -vn -clib=new ${OPT_CFLAGS} ${DEBUG_CFLAGS} ${HEADLESS_FONT_CFLAGS} ${PRINTF_CFLAGS} ${HEAP_CFLAGS} -create-app disk_tester.c disk_operations.c menu_system.c ui.c test_cards.c shared_strings.c intstate.asm -o "./$OUT_DIR/disk_tester" -m

# DSK build: bootable +3 disk image
zcc +zx -vn -clib=new ${OPT_CFLAGS} ${DEBUG_CFLAGS} ${HEADLESS_FONT_CFLAGS} ${PRINTF_CFLAGS} ${HEAP_CFLAGS} -subtype=plus3 -create-app disk_tester.c disk_operations.c menu_system.c ui.c test_cards.c shared_strings.c intstate.asm -o "./$OUT_DIR/disk_tester_plus3" -m

z88dk-dis "$OUT_DIR/disk_tester_CODE.bin" > "$OUT_DIR/disk_tester.asm" || true
