#pragma once

#include "common.h"
#include "chunk.h"

bool compile(vm_t *vm, const char *fname, const char *source, chunk_t *chunk);
