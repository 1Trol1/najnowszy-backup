gcc -c bootloader.c \
    -fno-pie -fno-pic \
    -mcmodel=large \
    -fno-stack-protector -fno-asynchronous-unwind-tables \
    -mno-red-zone -fshort-wchar \
    -I/usr/include/efi -I/usr/include/efi/x86_64 \
    -o bootloader.o

ld -nostdlib -znocombreloc -shared -Bsymbolic \
   -T /usr/lib/elf_x86_64_efi.lds \
   /usr/lib/crt0-efi-x86_64.o bootloader.o \
   -o bootloader.efi \
   -L/usr/lib -lgnuefi -lefi

objcopy \
  -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
  -j .rel -j .rela -j .rel.* -j .rela.* -j .reloc \
  --target=efi-app-x86_64 \
  bootloader.efi bootloader.efi.pe

mv bootloader.efi.pe bootloader.efi

objdump -x bootloader.efi | grep format
