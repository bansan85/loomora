/* Spike S11 (T132): the C interface between probe.m (Objective-C) and probe_cpp.cpp (C++11). */
#ifndef LOOMORA_PROBE_CPP_H
#define LOOMORA_PROBE_CPP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fills `out` (at most `size` bytes) with a text built by C++11 code and returns 20. */
int probe_cpp_run(char *out, unsigned long size);

#ifdef __cplusplus
}
#endif

#endif
