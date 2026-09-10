/* Pulled into every wolfSSL translation unit with -include, so that
   wolfcrypt/src/random.c sees a declaration for the seed function it is told
   to call via -DCUSTOM_RAND_GENERATE_SEED. The definition lives in the
   application; the linker resolves it there. */
#ifndef PSPRANDOM_DECL_H
#define PSPRANDOM_DECL_H
extern int psprandom_seed_raw(unsigned char *out, unsigned int sz);
#endif
