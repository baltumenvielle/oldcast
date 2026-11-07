// PC test for SipHash-2-4 against the reference test vectors (key 00..0f, message 00..n-1).
//   gcc -std=c11 -Wall -I../source test_siphash.c ../source/siphash.c -o test_siphash
//   ./test_siphash          prints "ALL PASSED" or the failing lengths
//   ./test_siphash hex      prints the hash of messages of length 0..63 for cross-checks
#include <stdio.h>
#include <string.h>

#include "siphash.h"

int main(int argc, char **argv)
{
    uint8_t key[16], msg[64];
    for (int i = 0; i < 16; i++)
        key[i] = (uint8_t)i;
    for (int i = 0; i < 64; i++)
        msg[i] = (uint8_t)i;

    if (argc > 1 && strcmp(argv[1], "hex") == 0) {
        for (int n = 0; n < 64; n++)
            printf("%016llx\n", (unsigned long long)siphash24(key, msg, n));
        return 0;
    }

    // From the SipHash paper and reference implementation.
    struct { int len; unsigned long long expected; } vectors[] = {
        {0, 0x726fdb47dd0e0e31ULL},
        {15, 0xa129ca6149be45e5ULL},
    };
    int failures = 0;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        unsigned long long got = siphash24(key, msg, vectors[i].len);
        if (got != vectors[i].expected) {
            printf("FAIL length %d: %016llx, expected %016llx\n", vectors[i].len, got, vectors[i].expected);
            failures++;
        }
    }
    puts(failures ? "FAILED" : "ALL PASSED");
    return failures != 0;
}
