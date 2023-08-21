import os

table = bytearray()
boot = bytearray()
files = bytearray()

block = 2

for fname in os.listdir("disk_files"):
    with open(os.path.join("disk_files", fname), "rb") as fh:
        if fname == "BOOT.bin":
            boot = bytearray(fh.read())
            if len(boot) > 508:
                raise Exception("BOOT.bin too large")
            while len(boot) != 508:
                boot.append(0)
            boot.extend([0x1a, 0xf9, 0x49, 0x33])

        else:
            nstore = bytearray()
            name = os.path.splitext(fname)[0]
            nstore.extend(map(ord, name))
            nstore.append(0)
            if len(nstore) > 24:
                raise Exception("filename too long")
            while len(nstore) != 24:
                nstore.append(0)

            table.extend(nstore)
            table.extend(block.to_bytes(4, byteorder="big"))
            contents = bytearray(fh.read())
            table.extend(len(contents).to_bytes(4, byteorder="big"))
            while len(contents) % 512 != 0:
                contents.append(0)

            files.extend(contents)
            block += len(contents) // 512

if len(table) > 512:
    raise Exception("too many files")
while len(table) != 512:
    table.append(0)

if not boot:
    raise Exception("no bootloader file")

with open("diskfile.img", "wb") as fh:
    fh.write(boot)
    fh.write(table)
    fh.write(files)
