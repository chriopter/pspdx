#ifndef PSPDX_ENTROPY_H
#define PSPDX_ENTROPY_H

/* The handshake is X25519 with ChaCha20-Poly1305, so it stands on a 128-bit
   security level: the ephemeral key is exactly as guessable as the entropy
   behind it, and nothing above 128 bits buys any strength. The pool below is
   a 20-byte SHA-1 state and cannot hold more than 160 bits however long the
   stick is swept, so a larger target here would only be a number on screen. */
#define ENTROPY_BITS 128

/* The sweep is scored on an invisible 250x250 field laid over the visible
   grid. Each point has one number, y * 250 + x, and each number pays out
   once: driving over ground already covered is worth nothing, which is what
   keeps a stick held against its stop from filling the bar. */
#define ENTROPY_FIELD_SIDE 250
#define ENTROPY_FIELD_COUNT (ENTROPY_FIELD_SIDE * ENTROPY_FIELD_SIDE)

/* One bit per new field, which is the conservative end of what was measured.
   The stick trace in testdata carries about 1100 bits of first-order entropy
   across 1202 newly entered fields; recoding a position as a field number
   cannot create entropy, so the true rate is at most ~0.9 bit per field. */
#define ENTROPY_BITS_PER_FIELD 1

void entropy_init(void);

/* Returns 1 if this field had not been entered before and was credited. */
int entropy_absorb_field(unsigned int field);

int entropy_bits(void);
int entropy_load(void);
void entropy_save(int replaying);
void entropy_forget(void);

int psprandom_seed_raw(unsigned char *seed, unsigned int size);

#endif
