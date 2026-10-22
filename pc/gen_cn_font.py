"""把界面要用的汉字渲染成单色点阵，合并成矩形，生成 src/cn_font.h 给 ESP32 用 fill 指令画。

屏里字库没有中文，ESP32 只能一块块画；矩形合并后每个字几十条 fill。
用法：
    python gen_cn_font.py              生成头文件
    python gen_cn_font.py --preview    在终端打印点阵预览
改了界面文字后把新字加进 CHARS 重新生成即可。依赖：pip install pillow
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

CHARS = "电脑监控内存显卡已用温度上传下载等待数据℃"
FONT = r"C:\Windows\Fonts\msyh.ttc"  # 微软雅黑常规，和屏上英文字库的细笔画一致
SIZE = 20       # 字号（像素）
SIZES = {"℃": 18}  # 个别符号单独定字号，跟在数字后面不要太抢眼
CELL = 22       # 每个字占的宽高
THRESHOLD = 90  # 灰度高于这个值算笔画
OUT = Path(__file__).resolve().parent.parent / "src" / "cn_font.h"


def render(ch, font):
    img = Image.new("L", (CELL, CELL), 0)
    d = ImageDraw.Draw(img)
    # 按字形实际外框居中
    l, t, r, b = d.textbbox((0, 0), ch, font=font)
    d.text(((CELL - (r - l)) / 2 - l, (CELL - (b - t)) / 2 - t), ch, fill=255, font=font)
    return [[img.getpixel((x, y)) > THRESHOLD for x in range(CELL)] for y in range(CELL)]


def to_rects(bits):
    """每行找连续段，上下行完全相同的段合并成一个矩形"""
    rects, open_ = [], {}  # (x0, x1) -> [x, y, w, h]
    for y in range(CELL + 1):
        runs = set()
        if y < CELL:
            x = 0
            while x < CELL:
                if bits[y][x]:
                    s = x
                    while x < CELL and bits[y][x]:
                        x += 1
                    runs.add((s, x))
                x += 1
        for key in list(open_):
            if key in runs:
                open_[key][3] += 1
                runs.discard(key)
            else:
                rects.append(open_.pop(key))
        for s, e in runs:
            open_[(s, e)] = [s, y, e - s, 1]
    return rects


def main():
    glyphs = []
    for ch in dict.fromkeys(CHARS):  # 去重保序
        bits = render(ch, ImageFont.truetype(FONT, SIZES.get(ch, SIZE)))
        if "--preview" in sys.argv:
            print(ch)
            print("\n".join("".join("#" if b else "." for b in row) for row in bits))
        glyphs.append((ch, to_rects(bits)))

    lines = [
        "// 由 pc/gen_cn_font.py 生成，不要手改",
        f"// 字体 {Path(FONT).name} {SIZE}px，字格 {CELL}x{CELL}",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define CN_CELL {CELL}",
        "",
        "struct CnGlyph {",
        "  const char *utf8;",
        "  uint16_t offset;  // 在 CN_RECTS 里的起始下标（每个矩形 4 字节 x,y,w,h）",
        "  uint8_t count;",
        "};",
        "",
        "static const uint8_t CN_RECTS[] = {",
    ]
    table, offset = [], 0
    for ch, rects in glyphs:
        flat = ", ".join(str(v) for r in rects for v in r)
        lines.append(f"  // {ch} {len(rects)} 个矩形")
        lines.append(f"  {flat},")
        table.append(f'  {{"{ch}", {offset}, {len(rects)}}},')
        offset += len(rects)
    lines += ["};", "", "static const CnGlyph CN_GLYPHS[] = {", *table, "};", ""]
    OUT.write_text("\n".join(lines), encoding="utf-8")

    total = sum(len(r) for _, r in glyphs)
    print(f"{len(glyphs)} 个字，共 {total} 个矩形，平均 {total / len(glyphs):.0f} 个/字 -> {OUT}")


if __name__ == "__main__":
    main()
