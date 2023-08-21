maps = {}

with open("keycodes.txt", "r") as fh:
    for line in fh.read().splitlines():
        line = line.split(" ")
        if line[1] == "SP":
            maps[int(line[0])] = (0x20, 0x20)
        elif line[1] == "EN":
            maps[int(line[0])] = (0x0a, 0x0a)
        elif line[1] == "BS":
            maps[int(line[0])] = (0x08, 0x08)
        elif line[1] == "SH":
            maps[int(line[0])] = (0xff, 0xff)
        else:
            maps[int(line[0])] = (ord(line[1]), ord(line[2]))

out = bytearray()
# num.to_bytes(breakdown[i] // 8, byteorder="big")

for code in range(136):  # ought to be enough
    if code in maps:
        out.append(maps[code][0])
        out.append(maps[code][1])
    else:
        out.append(0)
        out.append(0)

with open("disk_files/keymap.bin", "wb") as fh:
    fh.write(out)
