#!/usr/bin/env python3
"""Pack a Linux kernel Image + initrd (+ optional DTB) into a MEOW shim payload.

  mklinux.py KERNEL INITRD OUT [--dtb DTB] [--cmdline "..."]
           [--kernel-pa 0x88000000] [--initrd-pa 0x90000000]

Payload layout (little endian, all offsets from the payload start):

  0x000  char magic[8] = "MEOXLINUX"
  0x008  u32 version = 1, u32 flags
  0x010  u64 kernel_pa    2MB-aligned load address of the kernel Image
  0x018  u64 kernel_off
  0x020  u64 kernel_size
  0x028  u64 initrd_pa    0 = take the address from the DTB's /chosen
  0x030  u64 initrd_off
  0x038  u64 initrd_size
  0x040  u64 dtb_pa       0 = use the DTB ABL handed the shim
  0x048  u64 dtb_off
  0x050  u64 dtb_size
  0x058  char cmdline[1024]
  0x458  data

The kernel is entered at kernel_pa with x0 = device tree, x1-x3 = 0, which is
the arm64 Linux boot protocol. The shim writes the initrd location into
/chosen/linux,initrd-start and -end, and the command line into /chosen/bootargs.
"""
import argparse, struct

MAGIC = b'MEOXLINUX'
HDR = 0x458
CMDLINE = 0x58


def build(kernel, initrd, dtb, cmdline, kernel_pa, initrd_pa):
    k = open(kernel, 'rb').read()
    i = open(initrd, 'rb').read()
    d = open(dtb, 'rb').read() if dtb else b''

    if kernel_pa & 0x1FFFFF:
        raise SystemExit(f'kernel_pa {kernel_pa:#x} is not 2MB aligned')

    out = bytearray(HDR)
    off_k = len(out); out += k; out += b'\0' * (-len(out) & 0xf)
    off_i = len(out); out += i; out += b'\0' * (-len(out) & 0xf)
    off_d = len(out); out += d

    struct.pack_into('<8sII', out, 0, MAGIC, 1, 0)
    struct.pack_into('<9Q', out, 0x10,
                     kernel_pa, off_k, len(k),
                     initrd_pa, off_i, len(i),
                     0, off_d, len(d))
    cl = cmdline.encode()[:1023]
    out[CMDLINE:CMDLINE + len(cl)] = cl
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kernel')
    ap.add_argument('initrd')
    ap.add_argument('out')
    ap.add_argument('--dtb')
    ap.add_argument('--cmdline', default='console=ttyGS0')
    ap.add_argument('--kernel-pa', default='0x88000000')
    ap.add_argument('--initrd-pa', default='0x90000000')
    a = ap.parse_args()
    kernel_pa = int(a.kernel_pa, 16)
    initrd_pa = int(a.initrd_pa, 16)
    blob = build(a.kernel, a.initrd, a.dtb, a.cmdline, kernel_pa, initrd_pa)
    open(a.out, 'wb').write(blob)
    print(f'{a.out}: {len(blob)} bytes, kernel {kernel_pa:#x}, initrd {initrd_pa:#x}')


if __name__ == '__main__':
    main()
