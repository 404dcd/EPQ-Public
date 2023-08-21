#define _DEFAULT_SOURCE
#include "main.h"
#include "channel.h"
#include "disk.h"
#include "window.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define dprint(...)                     \
    do {                                \
        if (DEBUG) {                    \
            printf("L %d\t", __LINE__); \
            printf(__VA_ARGS__);        \
            printf("\n");               \
        }                               \
    } while (0)

/* Dump memory:
for (int i = 0; i < 100; i++) {
    printf("%02X ", mem[i]);
}

This emulator made for:
0x01 ADD
0x10 CPY
0x20 WRIVTR
0x30 JNSM
0x3c HLT
0xfe .8
0xff .16
*/

const int DEBUG = 0;
const uint32_t MAX_ADDRESS = 1 << 30;  // 1G
const uint32_t RESET_VECTOR = 0x10;

jmp_buf exception_during;
uint8_t* mem;
uint32_t pdbr = 0;
uint32_t ivtr = 0;
uint32_t flgr = 0;
uint32_t regs[16] = {0};
int force_no_page = 0;

int qints[32] = {0};
pthread_mutex_t qints_lock = PTHREAD_MUTEX_INITIALIZER;
struct Channel* to_screen;
struct Channel* to_disk;
struct Channel* from_kbd;
struct Channel* ints_to_main;

void regdump() {
    printf("\nAX 0x1 = %08x = %i = %u\n", regs[0x1], regs[0x1], regs[0x1]);
    printf("BX 0x2 = %08x = %i = %u\n", regs[0x2], regs[0x2], regs[0x2]);
    printf("CX 0x3 = %08x = %i = %u\n", regs[0x3], regs[0x3], regs[0x3]);
    printf("DX 0x4 = %08x = %i = %u\n", regs[0x4], regs[0x4], regs[0x4]);
    printf("EX 0x5 = %08x = %i = %u\n", regs[0x5], regs[0x5], regs[0x5]);
    printf("FX 0x6 = %08x = %i = %u\n", regs[0x6], regs[0x6], regs[0x6]);
    printf("GX 0x7 = %08x = %i = %u\n", regs[0x7], regs[0x7], regs[0x7]);
    printf("HX 0x8 = %08x = %i = %u\n", regs[0x8], regs[0x8], regs[0x8]);
    printf("IX 0x9 = %08x = %i = %u\n", regs[0x9], regs[0x9], regs[0x9]);
    printf("JX 0xa = %08x = %i = %u\n", regs[0xa], regs[0xa], regs[0xa]);
    printf("KX 0xb = %08x = %i = %u\n", regs[0xb], regs[0xb], regs[0xb]);
    printf("IM 0xc = %08x = %i = %u\n", regs[0xc], regs[0xc], regs[0xc]);
    printf("SP 0xd = %08x = %i = %u\n", regs[0xd], regs[0xd], regs[0xd]);
    printf("BP 0xe = %08x = %i = %u\n", regs[0xe], regs[0xe], regs[0xe]);
    printf("IP 0xf = %08x = %i = %u\n", regs[0xf], regs[0xf], regs[0xf]);
}

void memdump() {
    printf("\n00000000: ");
    for (int i = 0; i < 0x200; i++) {
        printf("%02X ", mem[i]);
        if (i % 16 == 15) {
            printf("\n%08x: ", i + 1);
        }
    }
    printf("\n");
}

uint32_t trans(uint32_t ptr) {
    if (ptr == 0) {
        dprint("  EEE mem addr = 0");
        longjmp(exception_during, 4 + 1);  // we need to transform exception codes to +1
    }

    if (flgr & (1 << 5) && !force_no_page) {  // if paging enabled
        if (pdbr == 0) {
            dprint("  EEE mem pdbr=0");
            longjmp(exception_during, 3 + 1);  // Unpaged address
        }
        uint32_t tmp = pdbr + (ptr >> 22) * 4;  // points to PDE
        tmp = (mem[tmp] << 24) + (mem[tmp + 1] << 16) + (mem[tmp + 2] << 8) + mem[tmp + 3];
        tmp = tmp & ~(0xfff);  // contains PT base
        if (tmp == 0) {
            dprint("  EEE mem pde=0");
            longjmp(exception_during, 3 + 1);  // Unpaged address
        }

        tmp += ((ptr >> 12) & 0x3ff) * 4;  // points to PTE
        tmp = (mem[tmp] << 24) + (mem[tmp + 1] << 16) + (mem[tmp + 2] << 8) + mem[tmp + 3];
        tmp = tmp & ~(0xfff);  // contains page base
        if (tmp == 0) {
            dprint("  EEE mem pte=0");
            longjmp(exception_during, 3 + 1);  // Unpaged address
        }

        tmp += ptr & 0xfff;
        if (tmp >= MAX_ADDRESS) {
            dprint("  EEE mem > max");
            longjmp(exception_during, 5 + 1);  // Address beyond max
        }
        return tmp;

    } else {
        if (ptr >= MAX_ADDRESS) {
            dprint("  EEE mem > max");
            longjmp(exception_during, 5 + 1);  // Address beyond max
        }
        return ptr;
    }
}

uint8_t get8b(uint32_t ptr) {
    return mem[trans(ptr)];
}
void set8b(uint32_t ptr, int8_t val) {
    mem[trans(ptr)] = val;
    return;
}

uint16_t get16b(uint32_t ptr) {
    return (mem[trans(ptr)] << 8) + mem[trans(ptr + 1)];
}
void set16b(uint32_t ptr, int16_t val) {
    mem[trans(ptr)] = val >> 8;
    mem[trans(ptr + 1)] = val & 0xff;
    return;
}

uint32_t get32b(uint32_t ptr) {
    return (mem[trans(ptr)] << 24) + (mem[trans(ptr + 1)] << 16) + (mem[trans(ptr + 2)] << 8) + mem[trans(ptr + 3)];
}
void set32b(uint32_t ptr, int32_t val) {
    mem[trans(ptr)] = val >> 24;
    mem[trans(ptr + 1)] = (val >> 16) & 0xff;
    mem[trans(ptr + 2)] = (val >> 8) & 0xff;
    mem[trans(ptr + 3)] = val & 0xff;
    return;
}

void param_resolve(uint32_t* val_r, uint32_t* addr_r, uint8_t* reg_r, uint8_t src_type, uint8_t nibs[20], int mode) {
    uint32_t val;
    uint32_t addr;
    uint32_t reg;

    switch (src_type) {
        case 0b0000:
            reg = nibs[0];
            val = regs[reg];
            break;
        case 0b0001:
            val = 0;
            for (int i = 0; i < mode / 4; i++) {
                val <<= 4;
                val += nibs[i];
            }
            break;
        case 0b0010:
            val = (nibs[0] << 4) + nibs[1];
            break;
        case 0b0011:
            addr = 0;
            for (int i = 0; i < 8; i++) {
                addr <<= 4;
                addr += nibs[i];
            }
            break;
        case 0b0100:
            addr = regs[nibs[0]];
            break;
        case 0b0101:
            addr = regs[nibs[0]] + (nibs[1] << 4) + nibs[2];
            break;
        case 0b0110:
            addr = regs[nibs[0]] - ((nibs[1] << 4) + nibs[2]);
            break;
        case 0b0111:
            addr = 0;
            for (int i = 0; i < 8; i++) {
                addr <<= 4;
                addr += nibs[i + 1];
            }
            addr += regs[nibs[0]];
            break;
        case 0b1000:
            addr = regs[nibs[0]] + regs[nibs[1]];
            break;
        case 0b1001:
            addr = regs[nibs[0]] + regs[nibs[1]] * 2;
            break;
        case 0b1010:
            addr = regs[nibs[0]] + regs[nibs[1]] * 4;
            break;
        case 0b1011:
            addr = regs[nibs[0]] + regs[nibs[1]] * 8;
            break;
        case 0b1100:
            addr = 0;
            for (int i = 0; i < 8; i++) {
                addr <<= 4;
                addr += nibs[i];
            }
            addr += regs[nibs[8]] + regs[nibs[9]];
            break;
        case 0b1101:
            addr = 0;
            for (int i = 0; i < 8; i++) {
                addr <<= 4;
                addr += nibs[i];
            }
            addr += regs[nibs[8]] + regs[nibs[9]] * 2;
            break;
        case 0b1110:
            addr = 0;
            for (int i = 0; i < 8; i++) {
                addr <<= 4;
                addr += nibs[i];
            }
            addr += regs[nibs[8]] + regs[nibs[9]] * 4;
            break;
        case 0b1111:
            addr = 0;
            for (int i = 0; i < 8; i++) {
                addr <<= 4;
                addr += nibs[i];
            }
            addr += regs[nibs[8]] + regs[nibs[9]] * 8;
            break;
    }
    *val_r = val;
    *addr_r = addr;
    *reg_r = reg;
    return;
}

int opc_is_srcdest(uint8_t opc) {
    switch (opc) {
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0c:
        case 0x0d:
        case 0x0e:
        case 0x0f:
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x16:
        case 0x19:
        case 0x37:
        case 0x38:
            return 1;
        default:
            return 0;
    }
}

int main() {
    to_screen = channel_make(32);
    to_disk = channel_make(32);
    from_kbd = channel_make(32);
    ints_to_main = channel_make(128);
    pthread_t window_thread;
    pthread_create(&window_thread, NULL, run_window, NULL);
    pthread_t disk_thread;
    pthread_create(&disk_thread, NULL, run_disk, NULL);

    uint64_t instrcount = 0;
    int param_nib_width[16] = {1, 0, 2, 8, 1, 3, 3, 9, 2, 2, 2, 2, 10, 10, 10, 10};

    size_t length = MAX_ADDRESS * sizeof(uint8_t);  // 1 gigabyte of memory
    mem = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        perror("mmap() failed");
        return 1;
    }

    FILE* fh = fopen("ROM.bin", "rb");
    if (fh == NULL) {
        perror("fopen(\"ROM.bin\") failed");
        return 1;
    }
    fseek(fh, 0, SEEK_END);  // determine length of file for fread
    int32_t fsize = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (fread(mem + RESET_VECTOR, fsize, 1, fh) != 1) {  // read into memory starting at 0x100
        printf("fread() failed");
        return 1;
    }
    fclose(fh);

    if (DEBUG) {
        memdump();
    }
    // begin emulation
    regs[0xf] = RESET_VECTOR;
    dprint("jumping to %08X", regs[0xf]);

    while (1) {
        instrcount += 1;
        // ================ HANDLE EXCEPTIONS ================
        int exno = setjmp(exception_during);
        if (exno != 0) {
            exno--;
            if (flgr & (1ul << 4)) {
                regs[0xd] -= 4;
                set32b(regs[0xd], flgr);
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0xf]);
                flgr &= ~(1ul << 4);
                if (ivtr == 0) {
                    dprint("  EEE unset IVTR with paging on");
                    longjmp(exception_during, 6 + 1);  // unregistered interrupt
                }
                force_no_page = 1;
                regs[0xf] = get32b(ivtr + exno * 4);  // jump
                force_no_page = 0;
                if (regs[0xf] == 0) {
                    dprint("  EEE retrieved handler address was 0");
                    longjmp(exception_during, 6 + 1);  // unregistered interrupt
                }

            } else {
                if (exno < 0x10) {  // we have exception but interrupts disabled
                    dprint("  -> exceptions disabled, halting");
                    goto done;
                } else {  // we never get hardware interrupts at this point if interrupts disabled
                    dprint("  -> interrupts disabled, ignoring");
                }
            }
        }

        // ================ DECODE PREF/OPC ================
        uint32_t curbyte = regs[0xf];  // tracks the next byte to read
        uint8_t opc = get8b(curbyte);
        curbyte++;
        // dprint("0x%08x <-IP opc=0x%02x", regs[0xf], opc);
        int mode = 32;
        if (opc == 0x17 || opc == 0x18 || opc == 0x00 || (opc > 0x3c && opc < 0xfe)) {
            dprint("  EEE bad opc");
            longjmp(exception_during, 1 + 1);  // invalid opcode
        }

        if (opc == 0xfe || opc == 0xff) {  // if we have a prefix
            if (opc == 0xfe) {
                mode = 8;
            } else {
                mode = 16;
            }

            opc = get8b(curbyte);  // enforce that new opc can be used with prefix
            curbyte++;
            // dprint("  ^- prefix, opc=0x%02x", opc);
            if (!((0x01 <= opc && opc <= 0x1b) || opc == 0x37)) {
                dprint("  EEE bad opc");
                longjmp(exception_during, 1 + 1);  // invalid opcode
            }
        }

        // ================ EXECUTE 0-OPERAND INSTRS ================
        int executed = 1;
        switch (opc) {
            case 0x1c: {  // PUSHR
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0x1]);
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0x2]);
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0x3]);
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0x4]);
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0x5]);
                regs[0xd] -= 4;
                set32b(regs[0xd], regs[0x6]);
                break;
            }
            case 0x1d: {  // POPR
                regs[0x6] = get32b(regs[0xd]);
                regs[0xd] += 4;
                regs[0x5] = get32b(regs[0xd]);
                regs[0xd] += 4;
                regs[0x4] = get32b(regs[0xd]);
                regs[0xd] += 4;
                regs[0x3] = get32b(regs[0xd]);
                regs[0xd] += 4;
                regs[0x2] = get32b(regs[0xd]);
                regs[0xd] += 4;
                regs[0x1] = get32b(regs[0xd]);
                regs[0xd] += 4;
                break;
            }
            case 0x22: {  // SETIEF
                flgr |= 1ul << 4;
                break;
            }
            case 0x23: {  // CLRIEF
                flgr &= ~(1ul << 4);
                break;
            }
            case 0x24: {  // SETVMF
                flgr |= 1ul << 5;
                break;
            }
            case 0x25: {  // CLRVMF
                flgr &= ~(1ul << 5);
                break;
            }
            case 0x36: {  // RET
                regs[0xf] = get32b(regs[0xd]);
                regs[0xd] += 4;
                break;
            }
            case 0x3a: {  // IRET
                regs[0xf] = get32b(regs[0xd]);
                regs[0xd] += 4;
                flgr = get32b(regs[0xd]);
                regs[0xd] += 4;
                break;
            }
            case 0x3b: {  // NOP
                break;
            }
            case 0x3c: {  // HLT
                if (flgr & (1ul << 4)) {
                    // if IEF
                    channel_block_until_data(ints_to_main);
                } else {
                    // else nothing could ever restart execution
                    dprint("  HLT with interrupts off");
                    goto done;
                }
                break;
            }
            default: {
                executed = 0;
                break;
            }
        }
        if (executed) {
            if (!(opc == 0x36 || opc == 0x3a)) {  // these modify IP already
                regs[0xf] = curbyte;
            }
            // goto is okay here because it essentially places the next few hundred
            // lines of code into a massive else {}
            goto interrupts;
        }

        // ================ GET PARAM TYPES, FETCH REST OF INSTR ================
        param_nib_width[1] = mode / 4;
        uint8_t src_type;
        uint8_t dst_type;
        uint8_t nib_buff[20];  // this is maximum we could theoretically read
        int dst_offset;
        int to_read;
        if (opc_is_srcdest(opc)) {  // src + dst present
            src_type = get8b(curbyte);
            curbyte++;
            dst_type = src_type & 0xf;
            src_type >>= 4;
            // max of one memory reference unless CPY
            if (src_type >= 0b0011 && dst_type >= 0b0011 && opc != 0x10) {
                dprint("  EEE two mem refs: 0x%02x, 0x%02x", src_type, dst_type);
                longjmp(exception_during, 2 + 1);  // illegal instruction
            }

            to_read = (param_nib_width[src_type] + param_nib_width[dst_type] + 1) / 2;
            for (int i = 0; i < to_read; i++) {
                uint8_t d = get8b(curbyte);
                curbyte++;
                nib_buff[2 * i] = d >> 4;
                nib_buff[2 * i + 1] = d & 0xf;
            }
            dst_offset = param_nib_width[src_type];
        } else {  // just dst present
            dst_type = get8b(curbyte);
            curbyte++;
            nib_buff[0] = dst_type & 0xf;
            dst_type >>= 4;
            to_read = param_nib_width[dst_type] / 2;  // no rounding up -> already read one nibble
            for (int i = 0; i < to_read; i++) {
                uint8_t d = get8b(curbyte);
                curbyte++;
                nib_buff[2 * i + 1] = d >> 4;
                nib_buff[2 * i + 2] = d & 0xf;
            }
            dst_offset = 0;  // dst_offset can be used as bool for src presence
        }

        // ================ LOAD SRC IF PRESENT ================
        uint32_t src_val;
        uint32_t src_addr;
        uint8_t src_reg;
        if (dst_offset) {
            // in-place function
            param_resolve(&src_val, &src_addr, &src_reg, src_type, nib_buff, mode);

            switch (opc) {
                // IN, OUT, src must be uimm8
                case 0x37:
                case 0x38:
                    if (src_type != 0x2) {
                        dprint("  EEE wanted src=uimm8 got 0x%01x", src_type);
                        longjmp(exception_during, 2 + 1);  // illegal instruction
                    }
                    break;
                // LMA, src must be memory reference
                case 0x19:
                    if (src_type <= 0x2) {
                        dprint("  EEE wanted src=[...] got 0x%01x", src_type);
                        longjmp(exception_during, 2 + 1);  // illegal instruction
                    }
                    break;
                // SWP, the src must not be immediate and it must not be the IP register
                case 0x11:
                    if (src_type == 0x1 || src_type == 0x2 || (src_type == 0x0 && src_reg == 0xf)) {
                        dprint("  EEE wanted src writable got 0x%01x", src_type);
                        longjmp(exception_during, 2 + 1);  // illegal instruction
                    }
                    break;
            }

            if (src_type >= 0x3 && opc != 0x19) {  // don't fetch source if LMA
                if (mode == 8) {
                    src_val = get8b(src_addr);
                } else if (mode == 16) {
                    src_val = get16b(src_addr);
                } else {
                    src_val = get32b(src_addr);
                }
            }
        }

        // ================ LOAD DESTINATION ================
        uint32_t dst_val;
        uint32_t dst_addr;
        uint8_t dst_reg;
        int write_dst = 1;
        // in-place function, starting indexing the nib buff at the right offset
        param_resolve(&dst_val, &dst_addr, &dst_reg, dst_type, nib_buff + dst_offset, mode);

        switch (opc) {
            // DSUB, DAND, PUSH, WRIVTR, WRPDBR, may be an immediate or use IP reg - bypassing default entirely
            case 0x03:
            case 0x07:
            case 0x1a:
            case 0x20:
            case 0x21:
                write_dst = 0;
                break;
            // GENINT, must be uimm8 - bypasses default check for non-immediate
            case 0x39:
                write_dst = 0;
                if (dst_type != 0x2) {
                    dprint("  EEE wanted dst=uimm8 got 0x%01x", dst_type);
                    longjmp(exception_during, 2 + 1);  // illegal instruction
                }
                break;
            // OUT, must use reg - this bypasses non-IP check in default which is right
            case 0x38:
                write_dst = 0;
                if (dst_type != 0x0) {
                    dprint("  EEE wanted dst=r got 0x%01x", dst_type);
                    longjmp(exception_during, 2 + 1);  // illegal instruction
                }
            default:
                // disallow both immediates and IP reg if not one of the above
                if (dst_type == 0x1 || dst_type == 0x2 || (dst_type == 0x0 && dst_reg == 0xf)) {
                    dprint("  EEE wanted dst writable got 0x%01x", dst_type);
                    longjmp(exception_during, 2 + 1);  // illegal instruction
                }
        }

        // all JXXX, CALL must use a memory reference
        if (0x26 <= opc && opc <= 0x35) {
            write_dst = 0;
            if (dst_type <= 0x2) {
                dprint("  EEE wanted dst=[...] got 0x%01x", dst_type);
                longjmp(exception_during, 2 + 1);  // illegal instruction
            }
        }

        // SNX, ZRX, LMA, IN must use reg (in addition to OUT, enforced earlier)
        if ((opc == 0x17 || opc == 0x18 || opc == 0x19 || opc == 0x37) && dst_type != 0x0) {
            dprint("  EEE wanted dst=r got 0x%01x", dst_type);
            longjmp(exception_during, 2 + 1);  // illegal instruction
        }

        if (dst_type >= 0x3) {
            // CPY, LMA, POP, CPFLGR, CPIVTR, JXXX, CALL, IN, don't need to read destination value
            int b = 0;
            b |= opc == 0x10 || opc == 0x19 || opc == 0x1b || opc == 0x1e || opc == 0x1f || opc == 0x37;
            b |= 0x26 <= opc && opc <= 0x35;
            if (!b) {
                if (mode == 8) {
                    dst_val = get8b(dst_addr);
                } else if (mode == 16) {
                    dst_val = get16b(dst_addr);
                } else {
                    dst_val = get32b(dst_addr);
                }
            }
        }

        // dprint("   src_type=0x%01x dst_type=0x%01x", src_type, dst_type);
        // dprint("   src_addr=0x%08x dst_addr=0x%08x", src_addr, dst_addr);
        // dprint("   src_reg=0x%01x dst_reg=0x%01x", src_reg, dst_reg);
        // dprint("   src_val=0x%08x dst_val=0x%08x", src_val, dst_val);

        // ================ EXECUTE 1-2 OPERAND INSTRUCTIONS ================
        uint32_t result;
        uint32_t modemask = (1ul << mode) - 1;
        int smf = flgr & 1;
        int cof = (flgr >> 1) & 1;
        int zrf = (flgr >> 2) & 1;
        int ngf = (ngf >> 3) & 1;
        int branch_taken = 0;
        switch (opc) {
            case 0x01: {  // ADD
                src_val &= modemask;
                dst_val &= modemask;
                result = (src_val + dst_val) & modemask;
                smf = ((result ^ src_val) & (result ^ dst_val)) >> (mode - 1);
                cof = result < src_val;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x02:  // SUB + DSUB
            case 0x03: {
                src_val &= modemask;
                dst_val &= modemask;
                result = (dst_val - src_val) & modemask;
                smf = ((dst_val ^ src_val) & (result ^ dst_val)) >> (mode - 1);
                cof = result > dst_val;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x04: {  // INC (ADD but s/src_val/1/g )
                dst_val &= modemask;
                result = (1 + dst_val) & modemask;
                smf = ((result ^ 1) & (result ^ dst_val)) >> (mode - 1);
                cof = result < 1;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x05: {  // DEC (SUB but s/src_val/1/g )
                dst_val &= modemask;
                result = (dst_val - 1) & modemask;
                smf = ((dst_val ^ 1) & (result ^ dst_val)) >> (mode - 1);
                cof = result > dst_val;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x06:  // AND + DAND
            case 0x07: {
                result = (src_val & dst_val) & modemask;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x08: {  // ORR
                result = (src_val | dst_val) & modemask;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x09: {  // XOR
                result = (src_val ^ dst_val) & modemask;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x0a: {  // NOT
                result = (~dst_val) & modemask;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x0b: {  // NEG (SUB but s/dst_val/0/g and s/src_val/dst_val/g)
                dst_val &= modemask;
                result = (0 - dst_val) & modemask;
                smf = ((0 ^ dst_val) & (result ^ 0)) >> (mode - 1);
                cof = result > 0;
                zrf = result == 0;
                ngf = result >> (mode - 1);
                break;
            }
            case 0x0c: {  // MUL
                src_val &= modemask;
                dst_val &= modemask;
                uint64_t full = (uint64_t)src_val * (uint64_t)dst_val;
                result = full & modemask;
                uint32_t higher = full >> mode;
                regs[0xc] &= ~modemask;
                regs[0xc] |= higher;  // write IM with top half
                cof = higher != 0;
                zrf = result == 0;
                break;
            }
            case 0x0d: {  // SML
                uint64_t a = src_val & (modemask >> 1);
                a -= src_val & (1ul << (mode - 1));
                uint64_t b = dst_val & (modemask >> 1);
                b -= dst_val & (1ul << (mode - 1));
                uint64_t full = a * b;
                result = full & modemask;
                uint32_t higher = (full >> mode) & modemask;
                regs[0xc] &= ~modemask;
                regs[0xc] |= higher;  // write IM with top half

                a = result & (modemask >> 1);  // just use a to check for COF
                a -= result & (1ul << (mode - 1));
                cof = a != full;
                zrf = result == 0;
                break;
            }
            case 0x0e: {  // DIV
                src_val &= modemask;
                dst_val &= modemask;
                if (src_val == 0) {
                    dprint("  EEE divide by zero");
                    longjmp(exception_during, 0 + 1);  // divide by zero
                }
                result = dst_val / src_val;
                uint32_t remainder = dst_val % src_val;
                regs[0xc] &= ~modemask;
                regs[0xc] |= remainder;  // write IM with remainder
                zrf = remainder == 0;
                break;
            }
            case 0x0f: {  // SDV
                uint32_t a = src_val & (modemask >> 1);
                a -= src_val & (1ul << (mode - 1));
                uint32_t b = dst_val & (modemask >> 1);
                b -= dst_val & (1ul << (mode - 1));
                if (src_val == 0) {
                    dprint("  EEE divide by zero");
                    longjmp(exception_during, 0 + 1);  // divide by zero
                }
                result = (int32_t)b / (int32_t)a;
                uint32_t remainder = (int32_t)b % (int32_t)a;
                regs[0xc] &= ~modemask;
                regs[0xc] |= remainder;  // write IM with remainder
                zrf = remainder == 0;
                cof = b == ~(modemask >> 1) && a == -1;  // if b is max negative
                break;
            }
            case 0x10: {  // CPY
                result = src_val;
                break;
            }
            case 0x11: {  // SWP
                result = src_val;
                if (src_type == 0x0) {
                    regs[src_reg] &= ~modemask;
                    regs[src_reg] |= dst_val & modemask;
                    regs[0] = 0;
                } else {
                    if (mode == 8) {  // casting uint32 down to uintX takes least sig bits
                        set8b(src_addr, dst_val);
                    } else if (mode == 16) {
                        set16b(src_addr, dst_val);
                    } else {
                        set32b(src_addr, dst_val);
                    }
                }
                break;
            }
            case 0x12: {  // ASR
                ngf = (dst_val >> (mode - 1)) & 1;
                if (src_val == 0) {
                    result = dst_val & modemask;
                    cof = 0;
                } else if (src_val > mode) {
                    result = 0ul - ngf;
                    cof = 0;
                } else {
                    result = (dst_val & modemask) >> src_val;
                    cof = (dst_val >> (src_val - 1)) & 1;
                    result -= (1ul & ngf) << (mode - src_val);
                }
                zrf = result == 0;
                break;
            }
            case 0x13: {  // BSR
                if (src_val == 0) {
                    result = dst_val & modemask;
                    cof = 0;
                } else if (src_val > mode) {
                    result = 0;
                    cof = 0;
                } else {
                    result = (dst_val & modemask) >> src_val;
                    cof = (dst_val >> (src_val - 1)) & 1;
                }
                zrf = result == 0;
                break;
            }
            case 0x14: {  // BSL
                if (src_val == 0) {
                    result = dst_val & modemask;
                    cof = 0;
                } else if (src_val > mode) {
                    result = 0;
                    cof = 0;
                } else {
                    result = (dst_val << src_val) & modemask;
                    cof = (dst_val >> (mode - src_val)) & 1;
                }
                zrf = result == 0;
                break;
            }
            case 0x15: {  // CSR
                src_val %= mode;
                result = ((dst_val >> src_val) | (dst_val << (mode - src_val))) & modemask;
                cof = (result >> (mode - 1)) && src_val != 0;
                zrf = result == 0;
                break;
            }
            case 0x16: {  // CSL
                src_val %= mode;
                result = ((dst_val << src_val) | (dst_val >> (mode - src_val))) & modemask;
                cof = result & 1;
                zrf = result == 0;
                break;
            }
            case 0x17: {        // SNX
                write_dst = 0;  // end-writing is prefix based but SNX behaves differently
                result = dst_val & (modemask >> 1);
                result -= dst_val & (1ul << (mode - 1));
                zrf = result == 0;
                ngf = result >> 31;
                regs[dst_reg] = result;
                regs[0] = 0;
                break;
            }
            case 0x18: {  // ZRX
                write_dst = 0;
                result = dst_val & modemask;
                zrf = result == 0;
                regs[dst_reg] = result;
                regs[0] = 0;
                break;
            }
            case 0x19: {  // LMA
                result = src_addr;
                break;
            }
            case 0x1a: {  // PUSH
                regs[0xd] -= mode / 8;
                if (mode == 8) {
                    set8b(regs[0xd], dst_val);
                } else if (mode == 16) {
                    set16b(regs[0xd], dst_val);
                } else {
                    set32b(regs[0xd], dst_val);
                }
                break;
            }
            case 0x1b: {  // POP
                if (mode == 8) {
                    result = get8b(regs[0xd]);
                } else if (mode == 16) {
                    result = get16b(regs[0xd]);
                } else {
                    result = get32b(regs[0xd]);
                }
                regs[0xd] += mode / 8;
                break;
            }
            case 0x1e: {  // CPFLGR
                result = flgr;
                break;
            }
            case 0x1f: {  // CPIVTR
                result = ivtr;
                break;
            }
            case 0x20: {  // WRIVTR
                ivtr = dst_val;
                break;
            }
            case 0x21: {  // WRPDBR
                pdbr = dst_val;
                break;
            }
            case 0x26: {  // JUMP
                branch_taken = 1;
                break;
            }
            case 0x27: {  // JAOE
                branch_taken = cof == 0;
                break;
            }
            case 0x28: {  // JABV
                branch_taken = cof == 0 && zrf == 0;
                break;
            }
            case 0x29: {  // JBOE
                branch_taken = cof == 1 || zrf == 1;
                break;
            }
            case 0x2a: {  // JBEL
                branch_taken = cof == 1;
                break;
            }
            case 0x2b: {  // JGOE
                branch_taken = smf == cof;
                break;
            }
            case 0x2c: {  // JGRA
                branch_taken = smf == cof && zrf == 0;
                break;
            }
            case 0x2d: {  // JLOE
                branch_taken = smf != cof || zrf == 1;
                break;
            }
            case 0x2e: {  // JLES
                branch_taken = smf != cof;
                break;
            }
            case 0x2f: {  // JSMM
                branch_taken = smf == 1;
                break;
            }
            case 0x30: {  // JNSM
                branch_taken = smf == 0;
                break;
            }
            case 0x31: {  // JZRO
                branch_taken = zrf == 1;
                break;
            }
            case 0x32: {  // JNZR
                branch_taken = zrf == 0;
                break;
            }
            case 0x33: {  // JPOS
                branch_taken = ngf == 0;
                break;
            }
            case 0x34: {  // JNEG
                branch_taken = ngf == 1;
                break;
            }
            case 0x35: {  // CALL
                branch_taken = 1;
                regs[0xd] -= 4;
                set32b(regs[0xd], curbyte);
                break;
            }
            case 0x37: {  // IN
                if (src_val == 0x3) {
                    result = channel_pop_default(from_kbd);
                }
                break;
            }
            case 0x38: {  // OUT
                if (src_val == 0x1) {
                    printf("%c", dst_val & 0xff);
                } else if (src_val == 0x2) {
                    channel_push(to_disk, dst_val);
                } else if (src_val == 0x4) {
                    channel_push(to_screen, dst_val);
                }
                break;
            }
            case 0x39: {  // GENINT
                if (dst_val < 0x15) {
                    dprint("  EEE cannot generate reserved software interrupt");
                    longjmp(exception_during, 2 + 1);  // illegal instruction
                }
                if (dst_val > 1023) {
                    dprint("  EEE interrupt number too large");
                    longjmp(exception_during, 2 + 1);  // illegal instruction
                }
                // We would longjmp here but we need to set the new PC.
                // The rest of this routine executes during interrupt checking, as that bit of
                // code can only be reached with this opc if it's a valid interrupt to be actioned.
                break;
            }
        }

        if (write_dst) {
            if (dst_type == 0x0) {
                regs[dst_reg] &= ~modemask;
                regs[dst_reg] |= result & modemask;
                regs[0] = 0;
            } else {
                if (mode == 8) {  // casting uint32 down to uintX takes least sig bits
                    set8b(dst_addr, result);
                } else if (mode == 16) {
                    set16b(dst_addr, result);
                } else {
                    set32b(dst_addr, result);
                }
            }
        }

        flgr &= ~(0b1111);
        flgr |= smf | (cof << 1) | (zrf << 2) | (ngf << 3);

        if (branch_taken) {
            regs[0xf] = dst_addr;
        } else {
            regs[0xf] = curbyte;
        }

        // ================ CHECK FOR INTERRUPTS ================
    interrupts:
        if (opc == 0x39) {
            // continue from where we left off, this a was valid software interrupt
            dprint("  III interrupt %02X", dst_val);
            longjmp(exception_during, dst_val + 1);
        }

        uint32_t hwint_code;
        if (flgr & (1ul << 4)) {  // only try to pop queue when paging on
            if (channel_pop_ornot(ints_to_main, &hwint_code)) {
                dprint("  III hardware interrupt %02X", hwint_code);
                longjmp(exception_during, hwint_code + 1);
            }
        }
    }

done:
    printf("\nStopping. Ran %lu cycles\n", instrcount);
    if (DEBUG) {
        memdump();
        regdump();
    }

    return 0;
}

/* General constraints:
        - max of one memory reference
        - src may be any mode and any register
        - destination must not be immediate (0001 or 0010)
        - destination as register must not be IP (1111)
        Exceptions:
        - src must not be immediate: SWP
        - src as register must not be IP: SWP
        - src must be uimm8 (0010): ASR, BSR, BSL, CSR, CSL, IN, OUT
        - src must be a memory reference: LMA
        - destination may be an immediate: DSUB, DAND, PUSH, WRIVTR, WRPDBR, GENINT
        - destination as register may use IP: DSUB, DAND, PUSH, WRIVTR, WRPDBR, OUT
        - destination must be a memory reference: all JXXX, CALL
        - destination must be uimm8 (0010): GENINT
        - destination must be register (0000): SNX, ZRX, LMA, IN, OUT
        - two memory references allowed: CPY
        */
