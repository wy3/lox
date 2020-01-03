#pragma once

#include "common.h"

typedef struct _obj obj_t;

typedef enum {
    VT_NIL,
    VT_BOOL,
    VT_NUM,
    VT_OBJ
} vtype_t;

typedef struct {
    vtype_t type;
    union {
        bool Bool : 1;
        double Num;
        obj_t *Obj;
        uint64_t raw;
    };
} val_t;

static const val_t VAL_NIL = { .type = VT_NIL };
static const val_t VAL_TRUE = { .type = VT_BOOL, .Bool = true };
static const val_t VAL_FALSE = { .type = VT_BOOL, .Bool = false };

#define VAL_BOOL(b)     ((val_t){ .type = VT_BOOL, .Bool = (b) })
#define VAL_NUM(n)      ((val_t){ .type = VT_NUM, .Num = (n) })
#define VAL_OBJ(o)      ((val_t){ .type = VT_OBJ, .o = (obj_t *)(o) })

#define AS_BOOL(v)      ((v).Bool)
#define AS_NUM(v)       ((v).Num)
#define AS_OBJ(v)       ((v).Obj)
#define AS_TYPE(v)      ((v).type)

#define IS_NIL(v)       (AS_TYPE(v) == VT_NIL)
#define IS_BOOL(v)      (AS_TYPE(v) == VT_BOOL)
#define IS_NUM(v)       (AS_TYPE(v) == VT_NUM)
#define IS_OBJ(v)       (AS_TYPE(v) == VT_OBJ)

#define IS_FALSEY(v)    (!(bool)(v).raw)

void val_print(val_t value);
bool val_equal(val_t a, val_t b);