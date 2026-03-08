set -e
mkdir -p out

zcc +zx -vn -create-app -lndos disk_tester.c intstate.asm -o ./out/disk_tester -m

z88dk-dis --target out/disk_tester > out/disk_tester.asm
