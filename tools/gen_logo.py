#!/usr/bin/env python3
"""生成 WHIPLASH 品牌 logo 的 1-bit 位图 C 数组（LVGL canvas 用）。"""
FONT = {
    'W': ["10001","10001","10001","10101","10101","11011","01010"],
    'H': ["10001","10001","10001","11111","10001","10001","10001"],
    'I': ["01110","00100","00100","00100","00100","00100","01110"],
    'P': ["11110","10001","10001","11110","10000","10000","10000"],
    'L': ["10000","10000","10000","10000","10000","10000","11111"],
    'A': ["01110","10001","10001","11111","10001","10001","10001"],
    'S': ["01111","10000","10000","01110","00001","00001","11110"],
}

TEXT = "WHIPLASH"
SCALE = 3      # 5x7 -> 15x21
GAP = 2        # 字间距
TEXT_COLOR = "COLOR_WHITE"

width = len(TEXT) * 5 * SCALE + (len(TEXT) - 1) * GAP
height = 7 * SCALE
stride = (width + 7) // 8

rows = [[0] * width for _ in range(height)]
x = 0
for ch in TEXT:
    glyph = FONT[ch]
    for gy, row_bits in enumerate(glyph):
        for gx, bit in enumerate(row_bits):
            if bit == '1':
                for sy in range(SCALE):
                    for sx in range(SCALE):
                        rows[gy * SCALE + sy][x + gx * SCALE + sx] = 1
    x += 5 * SCALE + GAP

bits = []
for y in range(height):
    for xb in range(stride):
        byte = 0
        for b in range(8):
            px = xb * 8 + b
            if px < width and rows[y][px]:
                byte |= 0x80 >> b
        bits.append(byte)

print(f"// {TEXT} logo: {width}x{height}, stride {stride}")
print(f"enum {{ BRAND_WIDTH = {width}, BRAND_HEIGHT = {height}, BRAND_STRIDE = {stride} }};")
print("static const uint8_t BRAND_BITS[] = {")
for i in range(0, len(bits), 13):
    print("    " + ", ".join(f"0x{b:02X}" for b in bits[i:i+13]) + ",")
print("};")
