#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vm.h"
#include "value.h"
#include "parser.h"
#include "object.h"

static void resetStack(vm_t *vm)
{
    vm->top = vm->stack;
    vm->frameCount = 0;
}

static void runtimeError(vm_t *vm, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fprintf(stderr, "Error: ");
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    for (int i = vm->frameCount - 1; i >= 0; i--) {
        frame_t *frame = &vm->frames[i];
        fun_t *function = frame->function;
        // -1 because the IP is sitting on the next instruction to be
        // executed.                                                 
        size_t instruction = frame->ip - function->chunk.code - 1;
        const char *fname = frame->function->chunk.source->fname;
        int line = CHUNK_GETLN(&frame->function->chunk, instruction);
        int column = CHUNK_GETCOL(&frame->function->chunk, instruction);
        fprintf(stderr, "[%s:%d:%d] in ", fname, line, column);
        if (function->name == NULL) {
            fprintf(stderr, "script\n");
        }
        else {
            fprintf(stderr, "%s()\n", function->name->chars);
        }
    }

    fflush(stderr);
    resetStack(vm);
}

vm_t *vm_create()
{
    vm_t *vm = malloc(sizeof(vm_t));
    if (vm == NULL) return NULL;

    memset(vm, '\0', sizeof(vm_t));
    vm->gc = malloc(sizeof(gc_t));
    vm->globals = malloc(sizeof(tab_t));
    vm->strings = malloc(sizeof(tab_t));

    gc_init(vm->gc);
    tab_init(vm->globals);
    tab_init(vm->strings);

    resetStack(vm);
    return vm;
}

void vm_close(vm_t *vm)
{
    if (vm == NULL) return;

    tab_free(vm->globals);
    tab_free(vm->strings);
    gc_free(vm->gc);

    free(vm->globals);
    free(vm->strings);
    free(vm->gc);

    free(vm);
}

vm_t *vm_clone(vm_t *from)
{
    vm_t *vm = malloc(sizeof(vm_t));
    if (vm == NULL) return NULL;

    memset(vm, '\0', sizeof(vm_t));

    vm->gc = from->gc;
    vm->globals = from->globals;
    vm->strings = from->strings;

    resetStack(vm);
    return vm;
}

#define PUSH(v)     *((vm)->top++) = (v)
#define POP()       *(--(vm)->top)
#define POPN(n)     *((vm)->top -= (n))
#define PEEK(i)     ((vm)->top[-1 - (i)])

static void defineNative(vm_t *vm, const char *name, cfn_t function)
{
    val_t native = VAL_CFN(function);
    val_t gname = VAL_OBJ(str_copy(vm, name, (int)strlen(name)));

    PUSH(gname);
    tab_set(vm->globals, AS_STR(gname), native);
    POP();
}

static val_t clockNative(vm_t *vm, int argc, val_t *args)
{
    return VAL_NUM((double)clock() / CLOCKS_PER_SEC);
}

static void concatenate(vm_t *vm)
{
    str_t *b = AS_STR(POP());
    str_t *a = AS_STR(POP());

    int length = a->length + b->length;
    char *chars = malloc((length + 1) * sizeof(char));
    memcpy(chars, a->chars, a->length);
    memcpy(chars + a->length, b->chars, b->length);
    chars[length] = '\0';

    str_t *result = str_take(vm, chars, length);
    PUSH(VAL_OBJ(result));
}

static bool prepareCall(vm_t *vm, fun_t *function, int argCount)
{
    if (argCount != function->arity) {
        runtimeError(vm, "Expected %d arguments but got %d.",
            function->arity, argCount);
        return false;
    }

    if (vm->frameCount == FRAMES_MAX) {
        runtimeError(vm, "Stack overflow.");
        return false;
    }

    frame_t *frame = &vm->frames[vm->frameCount++];
    frame->function = function;
    frame->ip = function->chunk.code;

    frame->slots = vm->top - argCount - 1;
    return true;
}

bool vm_call(vm_t *vm, val_t callee, int argCount)
{
    if (IS_OBJ(callee)) {
        switch (OBJ_TYPE(callee)) {
            case OT_FUN:
                return prepareCall(vm, AS_FUN(callee), argCount);

            default:
                // Non-callable object type.                   
                break;
        }
    }
    else if (IS_CFN(callee)) {
        cfn_t native = AS_CFN(callee);
        val_t result = native(vm, argCount, vm->top - argCount);
        vm->top -= argCount + 1;
        PUSH(result);
        return true;
    }

    runtimeError(vm, "Can only call functions and classes.");
    return false;
}

int vm_execute(vm_t *vm)
{
    register uint8_t *ip;
    register val_t *stack;
    register val_t *consts;
    register frame_t *frame;

#define STORE_FRAME() \
    frame->ip = ip

#define LOAD_FRAME() \
    frame = &vm->frames[vm->frameCount - 1]; \
	ip = frame->ip; \
    stack = frame->slots; \
    consts = frame->function->chunk.constants.values

#define STACK           (stack)
#define CONSTS          (consts)

#define PREV_BYTE()     (ip[-1])
#define READ_BYTE()     *(ip++)
#define READ_SHORT()    (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))

#define READ_CONST()    CONSTS[READ_BYTE()]
#define READ_STR()      AS_STR(READ_CONST())

#define ERROR(fmt, ...) \
    do { \
        STORE_FRAME(); \
        runtimeError(vm, fmt, ##__VA_ARGS__); \
        return VM_RUNTIME_ERROR; \
    } while (0)

#ifdef _MSC_VER
// Never try the 'computed goto' below on MSVC x86!
#if 0 //defined(_M_IX86) || (defined(_WIN32) && !defined(_WIN64))
#define INTERPRET       NEXT;
#define CODE(x)         _OP_##x:
#define CODE_ERR()      
#define NEXT            do { size_t i = READ_BYTE() * sizeof(size_t); __asm {mov ecx, [i]} __asm {jmp _jtab[ecx]} } while (0)
    static size_t _jtab[OPCODE_COUNT];
    if (_jtab[0] == 0) {
#define _CODE(x) __asm { mov _jtab[TYPE _jtab * OP_##x], offset _OP_##x }
        OPCODES();
#undef _CODE
    }
#else
#define INTERPRET       _loop: switch(READ_BYTE())
#define CODE(x)         case OP_##x:
#define CODE_ERR()      default:
#define NEXT            goto _loop
#endif
#else
#define INTERPRET       NEXT;
#define CODE(x)         _OP_##x:
#define CODE_ERR()      _err:
#define NEXT            goto *_jtab[READ_BYTE()]
#define _CODE(x)        &&_OP_##x,
    static void *_jtab[OPCODE_COUNT] = { OPCODES() };
#endif

    LOAD_FRAME();

    INTERPRET
    {
        CODE(PRINT) {
            int count = READ_BYTE();

            for (int i = count-1; i >= 0; i--) {
                val_print(PEEK(i));
                if (i > 0) printf("\t");
            }
            printf("\n");

            POPN(count);
            NEXT;
        }

        CODE(POP) {
            POP();
            NEXT;
        }

        CODE(NIL) {
            PUSH(VAL_NIL);
            NEXT;
        }

        CODE(TRUE) {
            PUSH(VAL_TRUE);
            NEXT;
        }

        CODE(FALSE) {
            PUSH(VAL_FALSE);
            NEXT;
        }

        CODE(CONST) {
            PUSH(READ_CONST());
            NEXT;
        }

        CODE(CALL) {
            int argCount = READ_BYTE();

            STORE_FRAME();
            if (!vm_call(vm, PEEK(argCount), argCount)) {
                return VM_RUNTIME_ERROR;
            }

            LOAD_FRAME();
            NEXT;
        }

        CODE(RET) {
            val_t result = POP();

            if (--vm->frameCount == 0) {
                POP();
                return VM_OK;
            }

            vm->top = frame->slots;
            PUSH(result);

            LOAD_FRAME();
            NEXT;
        }

        CODE(NOT) {
            PUSH(VAL_BOOL(IS_FALSEY(POP())));
            NEXT;
        }

        CODE(NEG) {
            switch (AS_TYPE(PEEK(0))) {
                case VT_BOOL:
                    PUSH(VAL_NUM(-(char)AS_BOOL(POP())));
                    NEXT;
                case VT_NUM:
                    PUSH(VAL_NUM(-AS_NUM(POP())));
                    NEXT;
            }
            ERROR("Operands must be a number/boolean.");
        }

        CODE(EQ) {
            val_t b = POP();
            val_t a = POP();
            PUSH(VAL_BOOL(val_equal(a, b)));
            NEXT;
        }

        CODE(LT) {
            switch (CMB_BYTES(AS_TYPE(PEEK(1)), AS_TYPE(PEEK(0)))) {
                case VT_NUM_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_BOOL(a < b));
                    NEXT;
                }
                case VT_BOOL_BOOL: {
                    char b = AS_BOOL(POP());
                    char a = AS_BOOL(POP());
                    PUSH(VAL_BOOL(a < b));
                    NEXT;
                }
                case VT_BOOL_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_BOOL(a < b));
                    NEXT;
                }
                case VT_NUM_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_BOOL(a < b));
                    NEXT;
                }
            }
            ERROR("Operands must be two numbers/booleans.");
        }

        CODE(LE) {
            switch (CMB_BYTES(AS_TYPE(PEEK(1)), AS_TYPE(PEEK(0)))) {
                case VT_NUM_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_BOOL(a <= b));
                    NEXT;
                }
                case VT_BOOL_BOOL: {
                    char b = AS_BOOL(POP());
                    char a = AS_BOOL(POP());
                    PUSH(VAL_BOOL(a <= b));
                    NEXT;
                }
                case VT_BOOL_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_BOOL(a <= b));
                    NEXT;
                }
                case VT_NUM_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_BOOL(a <= b));
                    NEXT;
                }
            }
            ERROR("Operands must be two numbers/booleans.");
        }

        CODE(ADD) {
            switch (CMB_BYTES(AS_TYPE(PEEK(1)), AS_TYPE(PEEK(0)))) {
                case VT_NUM_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a + b));
                    NEXT;
                }
                case VT_BOOL_BOOL: {
                    char b = AS_BOOL(POP());
                    char a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a + b));
                    NEXT;
                }
                case VT_BOOL_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a + b));
                    NEXT;
                }
                case VT_NUM_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a + b));
                    NEXT;
                }
                case VT_OBJ_OBJ:
                    if (IS_STR(PEEK(0)) && IS_STR(PEEK(1))) {
                        concatenate(vm);
                        NEXT;
                    }
            }
            ERROR("Operands must be two numbers/booleans/strings.");
        }

        CODE(SUB) {
            switch (CMB_BYTES(AS_TYPE(PEEK(1)), AS_TYPE(PEEK(0)))) {
                case VT_NUM_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a - b));
                    NEXT;
                }
                case VT_BOOL_BOOL: {
                    char b = AS_BOOL(POP());
                    char a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a - b));
                    NEXT;
                }
                case VT_BOOL_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a - b));
                    NEXT;
                }
                case VT_NUM_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a - b));
                    NEXT;
                }
            }
            ERROR("Operands must be two numbers/booleans.");
        }

        CODE(MUL) {
            switch (CMB_BYTES(AS_TYPE(PEEK(1)), AS_TYPE(PEEK(0)))) {
                case VT_NUM_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a * b));
                    NEXT;
                }
                case VT_BOOL_BOOL: {
                    char b = AS_BOOL(POP());
                    char a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a * b));
                    NEXT;
                }
                case VT_BOOL_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a * b));
                    NEXT;
                }
                case VT_NUM_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a * b));
                    NEXT;
                }
            }
            ERROR("Operands must be two numbers/booleans.");
        }

        CODE(DIV) {
            switch (CMB_BYTES(AS_TYPE(PEEK(1)), AS_TYPE(PEEK(0)))) {
                case VT_NUM_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a / b));
                    NEXT;
                }
                case VT_BOOL_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a / b));
                    NEXT;
                }
                case VT_BOOL_NUM: {
                    double b = AS_NUM(POP());
                    double a = AS_BOOL(POP());
                    PUSH(VAL_NUM(a / b));
                    NEXT;
                }
                case VT_NUM_BOOL: {
                    double b = AS_BOOL(POP());
                    double a = AS_NUM(POP());
                    PUSH(VAL_NUM(a / b));
                    NEXT;
                }
            }
            ERROR("Operands must be two numbers/booleans.");
        }

        CODE(DEF) {
            str_t *name = READ_STR();
            tab_set(vm->globals, name, PEEK(0));
            POP();
            NEXT;
        }

        CODE(GLD) {
            str_t *name = READ_STR();
            val_t value;      
            if (!tab_get(vm->globals, name, &value)) {
                ERROR("Undefined variable '%s'.", name->chars);
            }
            PUSH(value);
            NEXT;
        }

        CODE(GST) {
            str_t *name = READ_STR();
            if (tab_set(vm->globals, name, PEEK(0))) {
                tab_remove(vm->globals, name);
                ERROR("Undefined variable '%s'.", name->chars);
            }
            NEXT;
        }

        CODE(LD) {
            PUSH(STACK[READ_BYTE()]);
            NEXT;
        }

        CODE(ST) {
            STACK[READ_BYTE()] = PEEK(0);
            NEXT;
        }

        CODE(JMP) {
            uint16_t offset = READ_SHORT();
            ip += offset;
            NEXT;
        }

        CODE(JMPF) {
            uint16_t offset = READ_SHORT();
            if (IS_FALSEY(PEEK(0))) ip += offset;
            NEXT;
        }

        CODE(MAP) {
            uint8_t count = READ_BYTE();
            map_t *map = map_new(vm, 0, 0);

            for (val_t i = VAL_NUM(count - 1); AS_NUM(i) >= 0; AS_NUM(i) -= 1) {
                hash_set(&map->hash, AS_RAW(i), PEEK((int)AS_NUM(i)));
            }

            POPN(count);
            PUSH(VAL_OBJ(map));
            NEXT;
        }

        CODE(GET) {
            if (IS_MAP(PEEK(0))) {
                map_t *map = AS_MAP(PEEK(0));
                str_t *name = READ_STR();
                val_t value = VAL_NIL;
                tab_get(&map->table, name, &value);
                POP();
                PUSH(value);
            }
            else {
                ERROR("Operands must be a map.");
            }
            NEXT;
        }

        CODE(SET) {
            if (IS_MAP(PEEK(1))) {
                map_t *map = AS_MAP(PEEK(1));
                str_t *name = READ_STR();
                val_t value = PEEK(0);
                tab_set(&map->table, name, value);
                POP();
                POP();
                PUSH(value);
            }
            else {
                ERROR("Operands must be a map.");
            }
            NEXT;
        }

        CODE(GETI) {
            if (IS_MAP(PEEK(1))) {
                if (IS_NUM(PEEK(0))) {
                    map_t *map = AS_MAP(PEEK(1));
                    uint64_t key = AS_RAW(PEEK(0));
                    val_t value = VAL_NIL;
                    hash_get(&map->hash, key, &value);

                    POP();
                    POP();
                    PUSH(value);
                }
                else if (IS_STR(PEEK(0))) {
                    map_t *map = AS_MAP(PEEK(1));
                    str_t *key = AS_STR(PEEK(0));
                    val_t value = VAL_NIL;
                    tab_get(&map->table, key, &value);

                    POP();
                    POP();
                    PUSH(value);
                }
                else {
                    ERROR("Operands must be a number or string.");
                }
            }
            else {
                ERROR("Operands must be a map.");
            }
            NEXT;
        }

        CODE(SETI) {
            if (IS_MAP(PEEK(2))) {
                if (IS_NUM(PEEK(1))) {
                    map_t *map = AS_MAP(PEEK(2));
                    uint64_t key = AS_RAW(PEEK(1));
                    val_t value = POP();
                    hash_set(&map->hash, key, value);

                    POP();
                    POP();
                    PUSH(value);
                }
                else if (IS_STR(PEEK(1)))
                {
                    map_t *map = AS_MAP(PEEK(2));
                    str_t *key = AS_STR(PEEK(1));
                    val_t value = POP();
                    tab_set(&map->table, key, value);

                    POP();
                    POP();
                    PUSH(value);
                }
                else {
                    ERROR("Operands must be a number or string.");
                }
            }
            else {
                ERROR("Operands must be a map.");
            }
            NEXT;
        }

        CODE_ERR() {
            ERROR("Bad opcode, got %d!", PREV_BYTE());
        }
    }

    return VM_OK;
}

int vm_dofile(vm_t *vm, const char *fname)
{
    int result = VM_COMPILE_ERROR;
    src_t *source = src_new(fname);

    if (source != NULL) {
        fun_t *function = compile(vm, source);
        if (function == NULL) return VM_COMPILE_ERROR;

        val_t script = VAL_OBJ(function);

        PUSH(script);
        vm_call(vm, script, 0);

        result = vm_execute(vm);  
    }

    src_free(source);
    return result;
}

void set_global(vm_t *vm, const char *name, val_t value)
{
    val_t global = VAL_OBJ(str_copy(vm, name, (int)strlen(name)));

    PUSH(global);
    PUSH(value);
    tab_set(vm->globals, AS_STR(global), value);
    POP();
    POP();
}

void vm_push(vm_t *vm, val_t value)
{
    PUSH(value);
}

val_t vm_pop(vm_t *vm)
{
    return POP();
}
