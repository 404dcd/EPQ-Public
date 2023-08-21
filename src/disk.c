#include "disk.h"
#include "channel.h"
#include "main.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void* run_disk(void* args) {
    pthread_detach(pthread_self());

    FILE* fh = fopen("diskfile.img", "rb");
    if (fh == NULL) {
        perror("fopen(\"diskfile.img\") failed");
        pthread_exit(NULL);
    }

    fseek(fh, 0, SEEK_END);  // determine length of file
    int32_t fsize = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    uint8_t* diskbuf = malloc(fsize);

    if (fread(diskbuf, fsize, 1, fh) != 1) {
        printf("fread() failed");
        pthread_exit(NULL);
    }
    fclose(fh);

    while (1) {
        // block until next OUT instruction
        uint32_t sector = channel_pop_wait(to_disk);
        uint32_t addr = channel_pop_wait(to_disk);

        if (sector & (1 << 31)) {
            // writing
            sector &= ~(1 << 31);
            if (sector < (fsize / 512)) {
                memcpy(diskbuf + (sector * 512), mem + addr, 512);
            }
            channel_push(ints_to_main, 0x13);
        } else {
            // reading
            if (sector < (fsize / 512)) {
                memcpy(mem + addr, diskbuf + (sector * 512), 512);
            }
            channel_push(ints_to_main, 0x12);
        }
    }
}