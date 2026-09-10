#ifndef PSPDX_BENCH_H
#define PSPDX_BENCH_H

/* What the two TLS 1.3 ciphers cost on this CPU, measured rather than
   assumed: bulk throughput over eight megabytes in RAM, and a real
   handshake to the catalog host with each suite preferred. Runs when the
   rig asks (PSPDX.BENCH on the stick) and writes to the log. */
void bench_run(const char *url);

#endif
