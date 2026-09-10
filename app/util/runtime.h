#ifndef PSPDX_RUNTIME_H
#define PSPDX_RUNTIME_H

void logline(const char *fmt, ...);
void log_dump(void);
int log_count(void);
const char *log_at(int index);

unsigned now_ms(void);
int expired(unsigned start, unsigned budget_ms);

#endif
