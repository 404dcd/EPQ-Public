import os

REGS = ["zr", "ax", "bx", "cx", "dx", "ex", "fx", "gx",
        "hx", "ix", "jx", "kx", "im", "sp", "bp", "ip"]
OPCS = ["add", "sub", "dsub", "inc", "dec", "and", "dand", "orr", "xor", "not", "neg",
        "mul", "sml", "div", "sdv", "cpy", "swp", "asr", "bsr", "bsl", "csr", "csl", "snx",
        "zrx", "lma", "push", "pop", "pushr", "popr", "cpflgr", "cpivtr", "wrivtr", "wrpdbr",
        "setief", "clrief", "setvmf", "clrvmf", "jump", "jaoe", "jabv", "jboe", "jbel", "jgoe",
        "jgra", "jloe", "jles", "jsmm", "jnsm", "jzro", "jnzr", "jpos", "jneg", "call", "ret",
        "inp", "out", "genint", "iret", "nop", "hlt"]

param_bits = [
    (4,),
    (0,),
    (8,),
    (32,),
    (4,),
    (4, 8),
    (4, 8),
    (4, 32),
    (4, 4),
    (4, 4),
    (4, 4),
    (4, 4),
    (32, 4, 4),
    (32, 4, 4),
    (32, 4, 4),
    (32, 4, 4)
]


def decode_param(param):
    param = param.strip()
    if param in REGS:
        return 0x0, [REGS.index(param)]
    if "[" in param:
        param = param[1:-1]
        param = param.strip()
        if param in REGS:
            return 0x4, [REGS.index(param)]
        if "-" in param:
            p = param.split("-", 1)
            p[0], p[1] = p[0].strip(), p[1].strip()
            return 0x6, [REGS.index(p[0]), int(p[1], 0)]
        if "+" not in param:
            num = None
            try:
                num = int(param, 0)
            except:
                num = param
            return 0x3, [num]

        p = param.split("+")
        for x in range(len(p)):
            p[x] = p[x].strip()
        if len(p) == 2:
            if p[0] in REGS and p[1] in REGS:
                return 0x8, [REGS.index(p[0]), REGS.index(p[1])]
            if "*" in p[1]:
                s = p[1].split("*", 1)
                s[0], s[1] = s[0].strip(), s[1].strip()
                if s[1] == "2":
                    return 0x9, [REGS.index(p[0]), REGS.index(s[0])]
                if s[1] == "4":
                    return 0xa, [REGS.index(p[0]), REGS.index(s[0])]
                if s[1] == "8":
                    return 0xb, [REGS.index(p[0]), REGS.index(s[0])]

            num = None
            try:
                num = int(p[1], 0)
            except:
                return 0x7, [REGS.index(p[0]), p[1]]
            if num > 0xff:
                return 0x7, [REGS.index(p[0]), num]
            else:
                return 0x5, [REGS.index(p[0]), num]

        num = None
        try:
            num = int(p[0], 0)
        except:
            num = p[0]
        if p[1] in REGS and p[2] in REGS:
            return 0xc, [num, REGS.index(p[1]), REGS.index(p[2])]
        s = p[2].split("*", 1)
        s[0], s[1] = s[0].strip(), s[1].strip()
        if s[1] == "2":
            return 0xd, [num, REGS.index(p[1]), REGS.index(s[0])]
        if s[1] == "4":
            return 0xe, [num, REGS.index(p[1]), REGS.index(s[0])]
        if s[1] == "8":
            return 0xf, [num, REGS.index(p[1]), REGS.index(s[0])]

    num = None
    try:
        num = int(param, 0)
    except:
        return 0x1, [param]
    if num < 0 or num > 0xff:
        return 0x1, [num]
    else:
        return 0x2, [num]


def assemble(inpath, outpath):

    labels = {}
    datas = {}
    program = []
    end_data = bytearray()

    ip = 0

    with open(inpath, "r") as fh:
        for outln in fh:
            outln = outln.split(";", 1)[0].strip()
            if not outln:
                continue
            nested = []
            if outln[0] == "_":
                print("####", outln, "####")
                with open(os.path.join("source_files", outln), "r") as fh2:
                    nested = fh2.readlines()
            else:
                nested = [outln]

            for line in nested:
                line = line.split(";", 1)[0].strip()
                if not line:
                    continue
                if line[0] == "#":
                    line = line[1:].strip()
                    if line[0] == "+":
                        ip += int(line[1:].strip(), 0)  # offset address
                    else:
                        ip = int(line, 0)
                    continue

                if line[0] == ".":
                    labels[line.split(":", 1)[0]] = ip
                    print("%08X: %s" % (ip, line.split(":", 1)[0]))
                    continue
                if line[0] == "$":
                    name, v = line.split(" ", 1)
                    v = v[1:-1].encode().decode('unicode_escape').encode('latin-1')
                    datas[name] = len(end_data)
                    end_data.extend(v)
                    end_data.append(0)
                    continue

                # 0 mode, 1 opc, 2 src_type, 3 dst_type, 4 [src_args], 5 [dst_args]
                instr = [32, 0, -1, -1, [], []]
                lnsplit = line.split(None, 1)
                opc = lnsplit[0]
                ip += 1
                if "." in opc:
                    tmp = opc.split(".")
                    opc = tmp[0]
                    ip += 1
                    if tmp[1] == "8":
                        instr[0] = 8
                    elif tmp[1] == "16":
                        instr[0] = 16
                    else:
                        raise Exception("bad width suffix")

                if opc not in OPCS:
                    raise Exception("unrecognised opcode", opc)

                instr[1] = OPCS.index(opc) + 1
                if len(lnsplit) == 1:
                    program.append(instr)
                    continue

                param_bits[1] = (instr[0],)
                param_nibs = [sum(x)//4 for x in param_bits]
                params = lnsplit[1].split(",", 1)
                if len(params) == 1:
                    instr[3], instr[5] = decode_param(params[0])
                    ip += 1
                    ip += param_nibs[instr[3]] // 2
                else:
                    instr[2], instr[4] = decode_param(params[0])
                    instr[3], instr[5] = decode_param(params[1])
                    ip += 1
                    ip += (param_nibs[instr[2]] +
                           param_nibs[instr[3]] + 1) // 2
                program.append(instr)

            if len(nested) > 1:
                print("--------")

    with open(outpath, "wb") as fh:
        for instr in program:
            param_bits[1] = (instr[0],)
            buf = bytearray()
            if instr[0] == 8:
                buf.append(0xfe)
            if instr[0] == 16:
                buf.append(0xff)
            buf.append(instr[1])

            nibs = []
            if instr[2] != -1:
                nibs.append(instr[2] & 0xf)
            if instr[3] != -1:
                nibs.append(instr[3] & 0xf)
            if instr[2] != -1:
                breakdown = param_bits[instr[2]]
                for i in range(len(breakdown)):
                    num = instr[4][i]
                    if type(num) is str:
                        if num[0] == ".":
                            num = labels[num]
                        elif num[0] == "$":
                            num = datas[num] + ip
                        else:
                            raise Exception(
                                "bad specification of parameter", instr[4], num)
                    if breakdown[i] == 4:
                        nibs.append(num & 0xf)
                    else:
                        for b in num.to_bytes(breakdown[i] // 8, byteorder="big"):
                            nibs.append(b >> 4)
                            nibs.append(b & 0xf)

            if instr[3] != -1:
                breakdown = param_bits[instr[3]]
                for i in range(len(breakdown)):
                    num = instr[5][i]
                    if type(num) is str:
                        if num[0] == ".":
                            num = labels[num]
                        elif num[0] == "$":
                            num = datas[num] + ip
                        else:
                            raise Exception(
                                "bad specification of parameter", instr[5], num)
                    if breakdown[i] == 4:
                        nibs.append(num & 0xf)
                    else:
                        for b in num.to_bytes(breakdown[i] // 8, byteorder="big"):
                            nibs.append(b >> 4)
                            nibs.append(b & 0xf)

            if len(nibs) % 2 == 1:
                nibs.append(0)

            for x in range(len(nibs) // 2):
                buf.append((nibs[2 * x] << 4) + (nibs[2 * x + 1]))
            fh.write(buf)

        fh.write(end_data)


for fname in os.listdir("source_files"):
    inpath = os.path.join("source_files", fname)
    delextension = os.path.splitext(fname)[0]
    if fname[0] == "_":
        continue

    print("################", fname, "################")
    if fname == "ROM.txt":
        assemble(inpath, "ROM.bin")
    else:
        assemble(inpath, os.path.join("disk_files", delextension + ".bin"))

"""
# 0x10                  tells the assembler that 0x10 is the address the processor is loading the file at
$varname "contents"     $varname can be used as a label to refer to the null-term string contents
; comment               ignored by the assembler (can be inline too)
.label:                 .label will now translate to the address of the next instruction
_fn_myfunc.txt          replaces with the contents of _fn_myfunc.txt, path must start with underscore
"""
