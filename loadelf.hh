#include <list>
#include "interpret.hh"

#ifndef __LOAD_ELF_H__
#define __LOAD_ELF_H__

bool load_elf(const char* fn, state_t *ms);
bool is_rv64_elf(const char* fn);
bool is_alpha_elf(const char* fn);
bool load_alpha_elf(const char* fn, uint8_t *mem, uint64_t &entry);

#endif 

