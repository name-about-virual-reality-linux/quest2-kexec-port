#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "stb_image.h"

struct QkxxHeader {
    char magic[4];
    uint32_t num_frames;
    uint32_t fps;
    uint32_t width;
    uint32_t height;
};

int main(void) {
    int fd = open("/testvideo.qkxv", O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(fd, &st);
    uint8_t *data = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    struct QkxxHeader *hdr = (struct QkxxHeader *)data;
    uint32_t *sizes = (uint32_t *)(data + sizeof(struct QkxxHeader));
    uint8_t *frames = (uint8_t *)(sizes + hdr->num_frames);

    printf("Header: num=%u, fps=%u, w=%u, h=%u, frame0_size=%u\n",
           hdr->num_frames, hdr->fps, hdr->width, hdr->height, sizes[0]);

    int w = 0, h = 0, comp = 0;
    uint8_t *dec = stbi_load_from_memory(frames, sizes[0], &w, &h, &comp, 4);
    if (!dec) {
        printf("stbi_load_from_memory FAILED: %s\n", stbi_failure_reason());
        return 1;
    }
    printf("SUCCESS! Decoded frame 0: %dx%d channels=%d, pixel0=[%u, %u, %u, %u]\n",
           w, h, comp, dec[0], dec[1], dec[2], dec[3]);
    stbi_image_free(dec);
    return 0;
}
