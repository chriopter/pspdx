#ifndef PSPDX_RUNTIME_H
#define PSPDX_RUNTIME_H

void logline(const char *fmt, ...);
void log_dump(void);

/* The same, from a thread of its own below everything else: a frame does
   not wait for the stick. */
void log_dump_later(void);
int log_count(void);
const char *log_at(int index);

unsigned now_ms(void);
unsigned now_us(void);
int expired(unsigned start, unsigned budget_ms);

#endif
