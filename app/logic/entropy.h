#ifndef PSPDX_ENTROPY_H
#define PSPDX_ENTROPY_H

#define ENTROPY_BITS 256

void entropy_init(void);
void entropy_absorb_motion(unsigned char lx, unsigned char ly, float x, float y);
int entropy_bits(void);
int entropy_load(void);
void entropy_save(int replaying);
void entropy_forget(void);

int psprandom_seed_raw(unsigned char *seed, unsigned int size);

#endif
