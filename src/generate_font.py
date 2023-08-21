from PIL import Image

data = bytearray(Image.open("font.bmp").getdata())

with open("disk_files/font.bin", "wb") as fh:
    fh.write(data)
