set -e
mkdir -p out

zcc +zx -vn -create-app -lndos disk_tester.c -o ./out/disk_tester