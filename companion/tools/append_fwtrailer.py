# PlatformIO post-build hook: append a content-fingerprint trailer to firmware.bin
# so the ESP can self-update from the SD by content (no version number), like the
# sd2snes MCU bootloader that re-flashes firmware.imX when it differs (see ota.cpp).
#
#   [ ... firmware image ... ][ "SD2ESPFW" (8) | crc32(image) u32 LE (4) | pad (4) ]
#
# The CRC is over the image bytes only. The ESP bootloader/Update use the image's
# own declared length and ignore the trailer, so the file still flashes/boots fine.
Import("env")
import os, zlib, struct

MAGIC = b"SD2ESPFW"
TRAILER = 16   # magic(8) + crc32(4) + pad(4)

def append_trailer(source, target, env):
    binpath = os.path.join(env.subst("$BUILD_DIR"), env.subst("$PROGNAME") + ".bin")
    if not os.path.exists(binpath):
        return
    data = bytearray(open(binpath, "rb").read())
    # idempotent: strip a trailer left by a previous run -> pure image
    if len(data) >= TRAILER and bytes(data[-TRAILER:-TRAILER + 8]) == MAGIC:
        data = data[:-TRAILER]
    crc = zlib.crc32(bytes(data)) & 0xffffffff
    data += MAGIC + struct.pack("<I", crc) + b"\x00\x00\x00\x00"
    open(binpath, "wb").write(data)
    print("fw trailer appended: crc=0x%08x (+%d bytes)" % (crc, TRAILER))

env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", append_trailer)
