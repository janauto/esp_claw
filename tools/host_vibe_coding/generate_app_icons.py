"""Export the firmware's geometric app artwork for inspection or SD packages."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageEnhance
from app_package import image_to_bin


def render():
    scale = 4
    image = Image.new("RGB", (256 * scale, 256 * scale), "black")
    draw = ImageDraw.Draw(image)

    def rect(x, y, w, h, radius, color):
        draw.rounded_rectangle((x * scale, y * scale, (x + w) * scale - 1, (y + h) * scale - 1),
                               radius=radius * scale, fill=color)

    rect(14, 14, 228, 228, 26, "#EDC22E")
    rect(26, 58, 204, 172, 14, "#173B3A")
    for i in range(3):
        rect(34 + i * 20, 30, 10, 10, 5, "#173B3A")
    for i in range(7):
        rect(64 + (i if i <= 3 else 6 - i) * 12, 96 + i * 12, 16, 16, 3, "#FFF0B3")
    rect(136, 168, 58, 14, 5, "#69D6B0")
    return image.resize((256, 256), Image.Resampling.LANCZOS)


if __name__ == "__main__":
    destination = Path(__file__).parent / "assets"
    destination.mkdir(exist_ok=True)
    selected = render()
    for state, image in (("selected", selected), ("unselected", ImageEnhance.Brightness(selected).enhance(0.42))):
        image.save(destination / f"app_{state}.png")
        (destination / f"app_{state}.bin").write_bytes(image_to_bin(image))
