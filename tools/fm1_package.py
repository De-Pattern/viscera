# SPDX-License-Identifier: MIT
# Python port of M-VAVE's sdk/Package.cs (FM1 Open V14): builds an FM-1 .fwsc
# update package from the linked application image.
# Copyright (c) 2023 Andrey Grigoryev (kagaimiq), 2026 M-VAVE (original C#).
# JieLi formats and ciphers: jl-misctools 0a5b12d. Port: Viscera.
import os, re

AREA, ENTRY_POINT, APP_DATA = 0x4000, 0x02000120, 0x120
FLASH_LIMIT, USER_SIZE, BT_SIZE, MINIMUM_VM = 0xff000, 536576, 4096, 32768
PUBLIC_CODE = 0x980f

def _table():
    t = []
    for i in range(256):
        v = i << 8
        for _ in range(8):
            v = ((v << 1) ^ (0x1021 if v & 0x8000 else 0))
        t.append(v & 0xffff)
    return t
CRC_TABLE = _table()

def crc(b, begin, length):
    v = 0
    for i in range(begin, begin + length):
        v = ((v << 8) ^ CRC_TABLE[(v >> 8) ^ b[i]]) & 0xffff
    return v

def cipher(b, begin, length, state):
    for i in range(begin, begin + length):
        b[i] ^= state & 0xff
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xffff

def round_up(size, a):
    return (size + a - 1) & -a

class Image:
    def __init__(self, size, fill):
        self.b = bytearray([fill]) * size
    def number(self, off, value, width):
        for n in range(width):
            self.b[off + n] = (value >> (8 * n)) & 0xff
    def copy(self, off, data):
        self.b[off:off + len(data)] = data
    def text(self, off, s):
        self.copy(off, s.encode('ascii'))
    def checksum(self, field, off, size):
        self.number(field, crc(self.b, off, size), 2)
    def record(self, off, name, crcv, address, size, attributes, last, extra):
        self.b[off:off + 32] = bytes(32)
        self.number(off + 2, crcv, 2); self.number(off + 4, address, 4); self.number(off + 8, size & 0xffffffff, 4)
        self.number(off + 12, attributes, 2); self.number(off + 14, last, 2)
        for n in range(16, 32):
            self.b[off + n] = 255
        self.text(off + 16, name); self.b[off + 16 + len(name)] = 0
        if extra is not None:
            self.number(off + 28, extra, 4)
        self.checksum(off, off + 2, 30)

def read_resource(directory, name, size):
    b = open(os.path.join(directory, name), 'rb').read()
    if len(b) != size:
        raise ValueError('Unexpected FM1 resource: ' + name)
    return b

def u16(b, o): return b[o] | b[o + 1] << 8
def i32(b, o): return int.from_bytes(b[o:o + 4], 'little', signed=True)

def encode_boot(plain):
    result = bytearray(plain)
    banks = u16(plain, 0)
    if banks < 1 or banks > len(plain) // 16:
        raise ValueError('Invalid boot directory.')
    prev_end = banks * 16
    for i in range(banks):
        h = i * 16
        length, start = u16(plain, h + 2), i32(plain, h + 8)
        if (length == 0 or start < prev_end or start > len(plain) - length or
                crc(plain, h, 14) != u16(plain, h + 14) or crc(plain, start, length) != u16(plain, h + 12)):
            raise ValueError('Invalid boot bank.')
        cipher(result, h, 16, 65535); cipher(result, start, length, 65535); prev_end = start + length
    return bytes(result)

def build_flash(app, directory, end, vm_end):
    boot = read_resource(directory, 'uboot.boot', 14488)
    config = read_resource(directory, 'boot-config.bin', 721)
    tool = read_resource(directory, 'cfg_tool.bin', 383)
    image, settings = Image(end, 255), Image(34 + len(config), 0)
    for bit in range(16):
        settings.b[bit] = 16
        settings.b[16 + bit] = 16 ^ (255 if ((PUBLIC_CODE >> bit) & 1) == 0 else 0)
    settings.checksum(32, 0, 32); settings.copy(34, config)
    image.copy(0, bytes([0, 0, 0x20, 2, 0x2f, 0x10, 0x4c, 0xc9, 0, 0xf0, 0x0f, 0, 0x10, 0x10, 0xa4, 0xff,
                         0x4e, 0x5d, 0x0b, 0x41, 0xe0, 0xcd, 0x78, 0x1d, 0xc8, 0x56, 0x41, 0x7f, 0xb3, 0x67, 0xcf, 0x9f]))
    image.checksum(0, 2, 30)
    config_at = 160 + len(boot)
    image.record(32, 'uboot.boot', crc(boot, 0, len(boot)), 160, len(boot), 0, 0, None)
    image.record(64, 'isd_config.ini', crc(settings.b, 0, len(settings.b)), config_at, len(settings.b), 0x8002, 0, None)
    image.record(96, 'app_dir_head', 65535, AREA, 0xffffffff, 0xff81, 0, None)
    image.record(128, 'key_mac', 65535, FLASH_LIMIT, 4096, 0x0112, 65535, None)
    for p in range(0, 160, 32):
        cipher(image.b, p, 32, 65535)
    image.copy(160, encode_boot(boot)); image.copy(config_at, settings.b)
    payload = APP_DATA + len(app) + len(tool)
    a = AREA + APP_DATA
    image.copy(a, app); image.copy(a + len(app), tool)
    image.record(AREA + 32, 'app.bin', crc(app, 0, len(app)), APP_DATA, len(app), 0xff82, 0, None)
    image.record(AREA + 64, 'cfg_tool.bin', crc(tool, 0, len(tool)), APP_DATA + len(app), len(tool), 0xff82, 0, None)
    image.record(AREA + 96, 'VM', 65535, end, vm_end - end, 0x8112, 0, MINIMUM_VM)
    image.record(AREA + 128, 'PRCT', 65535, 0, end, 0x8292, 0, 0x45444f43)
    image.record(AREA + 160, 'BTIF', 65535, vm_end, BT_SIZE, 0x8192, 1, 0x4f545541)
    image.record(AREA, 'app_area_head', crc(image.b, AREA + 32, payload - 32), ENTRY_POINT, payload, 0xff83, 0, None)
    reserved = AREA + payload
    image.record(reserved + 32, 'USER', 65535, FLASH_LIMIT - USER_SIZE, USER_SIZE, 0x8192, 1, 0x4f545541)
    image.record(reserved, 'EXT_RESERVED', crc(image.b, reserved + 32, 32), 32, 64, 0xff93, 1, None)
    for p in range(0, payload + 64, 32):
        cipher(image.b, AREA + p, min(32, payload + 64 - p), PUBLIC_CODE ^ (p >> 2))
    return image

def wrap(flash, loader, product):
    if not 1 <= len(loader) < 32768:
        raise ValueError('Invalid FM1 update loader.')
    loader_at = 1024 + round_up(len(flash.b), 32)
    total = loader_at + round_up(len(loader), 32)
    ufw = Image(total, 255)
    ufw.b[0:224] = bytes(224)
    files, names = [bytes(flash.b), loader], ['flash.bin', 'ota.bin']
    for i, f in enumerate(files):
        r = 64 + i * 80
        ufw.number(r, 0 if i == 0 else 100, 2); ufw.number(r + 2, i, 2)
        ufw.number(r + 4, crc(f, 0, len(f)), 2)
        ufw.number(r + 8, 1024 if i == 0 else loader_at, 4)
        ufw.number(r + 12, len(f), 4); ufw.number(r + 16, round_up(len(f), 32), 4)
        ufw.text(r + 64, names[i]); cipher(ufw.b, r, 80, 65535)
    ufw.number(4, total, 4); ufw.number(8, 2, 2); ufw.number(10, 4, 2); ufw.number(12, 512, 4)
    ufw.text(16, 'AC791N'); ufw.checksum(2, 64, 160); ufw.checksum(0, 2, 62); cipher(ufw.b, 0, 64, 65535)
    ufw.copy(1024, flash.b); ufw.copy(loader_at, loader)
    out = bytearray()
    for src in range(total):
        out.append(ufw.b[src])
        if src < 940 and (src + 1) % 47 == 0:
            idx = src // 47
            out.append((ord(product[idx]) + idx + 1) & 0xff if idx < len(product) else 125)
    return bytes(out)

def create(app, resources, output, user_bytes, product):
    if not re.fullmatch(r'FM-1_8[0-9]{6}', product or ''):
        raise ValueError('Invalid FM1 update identity.')
    if not app or len(app) > FLASH_LIMIT - USER_SIZE - BT_SIZE - MINIMUM_VM - AREA - APP_DATA - 383 - 64 or user_bytes != USER_SIZE:
        raise ValueError('Application does not fit the FM1 layout.')
    image_size = round_up(AREA + APP_DATA + len(app) + 383 + 64, 4096)
    user_at = FLASH_LIMIT - USER_SIZE
    vm_end = user_at - BT_SIZE
    if image_size > vm_end - MINIMUM_VM:
        raise ValueError('Insufficient FM1 settings space.')
    image = build_flash(app, resources, image_size, vm_end)
    pkg = wrap(image, open(os.path.join(resources, 'ota.bin'), 'rb').read(), product)
    open(output, 'wb').write(pkg)
    return {'FLASH_BIN_SIZE': image_size, 'VM_START_ADDR': image_size, 'VM_END_ADDR': vm_end,
            'VM_REAL_SIZE': vm_end - image_size, 'vm_extra_gap_bytes': vm_end - image_size - MINIMUM_VM,
            'app_bytes': len(app), 'app_limit': FLASH_LIMIT - USER_SIZE - BT_SIZE - MINIMUM_VM - AREA - APP_DATA - 383 - 64}
