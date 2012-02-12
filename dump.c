void dump_buf(unsigned char *data, uint32_t len) {
    char nonprint_char = '.';
    if (len > 0) {
        unsigned char *str = data;
        uint8_t width = 16;
        uint32_t i = 0,
                 j = 0;

        while (i < len) {
            printf("  ");

            for (j = 0; j < width; j++) {
                if (i + j < len)
                    printf("%02x ", str[j]);
                else printf("   ");

                if ((j+1) % (width/2) == 0)
                    printf("   ");
            }

            for (j = 0; j < width; j++)
                if (i + j < len)
                    printf("%c", isprint(str[j]) ? str[j] : nonprint_char);
                else printf(" ");

            str += width;
            i   += j;

            printf("\n");
        }
    }
    fflush(stdout);
}
