#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "lexer.h"
#include "object.h"

typedef struct _parser   parser_t;
typedef struct _compiler compiler_t;

struct _parser {
    vm_t *vm;
    chunk_t *compilingChunk;
    lexer_t *lexer;
    compiler_t *compiler;
    tok_t current;
    tok_t previous;
    bool hadError;
    bool panicMode;
};

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,  // =        
    PREC_OR,          // or       
    PREC_AND,         // and      
    PREC_EQUALITY,    // == !=    
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -      
    PREC_FACTOR,      // * /      
    PREC_UNARY,       // ! -      
    PREC_CALL,        // . ()     
    PREC_PRIMARY
} prec_t;

typedef void (* parsefn_t)(parser_t *parser, bool canAssign);

typedef struct {
    parsefn_t prefix;
    parsefn_t infix;
    prec_t precedence;
} rule_t;

typedef struct {
    tok_t name;
    int depth;
} local_t;

struct _compiler
{
    local_t locals[UINT8_COUNT];
    int localCount;
    int scopeDepth;
};

static chunk_t *currentChunk(parser_t *parser)
{
    return parser->compilingChunk;
}

static void errorAt(parser_t *parser, tok_t *token, const char *message)
{
    if (parser->panicMode) return;
    parser->panicMode = true;

    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    }
    else if (token->type == TOKEN_ERROR) {
        // Nothing.                                                
    }
    else {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    parser->hadError = true;
}

static void error(parser_t *parser, const char *message)
{
    errorAt(parser, &parser->previous, message);
}

static void errorAtCurrent(parser_t *parser, const char *message)
{
    errorAt(parser, &parser->current, message);
}

static void advance(parser_t *parser)
{
    parser->previous = parser->current;

    for (;;) {
        parser->current = lexer_scan(parser->lexer);
        if (parser->current.type != TOKEN_ERROR) break;

        errorAtCurrent(parser, parser->current.start);
    }
}

static void consume(parser_t *parser, toktype_t type, const char* message)
{
    if (parser->current.type == type) {
        advance(parser);
        return;
    }

    errorAtCurrent(parser, message);
}

static bool check(parser_t *parser, toktype_t type)
{
    return parser->current.type == type;
}

static bool match(parser_t *parser, toktype_t type)
{
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

static void emitByte(parser_t *parser, uint8_t byte)
{
    chunk_emit(currentChunk(parser), byte,
        parser->previous.line, parser->previous.column);
}

static void emitBytes(parser_t *parser, uint8_t byte1, uint8_t byte2)
{
    emitByte(parser, byte1);
    emitByte(parser, byte2);
}

static void emitNBytes(parser_t *parser, void *bytes, size_t size)
{
    const uint8_t *bs = bytes;
    for (size_t i = 0; i < size; i++) {
        emitByte(parser, bytes == NULL ? 0 : bs[i]);
    }
}

static void emitReturn(parser_t *parser)
{
    emitByte(parser, OP_RET);
}

static uint16_t makeConstant(parser_t *parser, val_t value)
{
    int constant = arr_add(&currentChunk(parser)->constants, value, false);
    if (constant > UINT16_MAX) {
        error(parser, "Too many constants in one chunk.");
        return 0;
    }

    return (uint16_t)constant;
}

static void emitBytesAndConstLong(parser_t *parser, uint8_t op, int arg)
{
#define CHANGE(x)   (op == x) op = x##L
    if (arg > UINT8_MAX) {
        if CHANGE(OP_GLD);
        if CHANGE(OP_GST);
        if CHANGE(OP_DEF);
        if CHANGE(OP_CONST);
        emitByte(parser, op);
        emitBytes(parser, (arg >> 8) & 0xFF, arg & 0xFF);
        return;
    }
#undef CHANGE
    emitBytes(parser, op, (uint8_t)arg);
}

static void emitConstant(parser_t *parser, val_t value)
{
    uint16_t constant = makeConstant(parser, value);
    emitBytesAndConstLong(parser, OP_CONST, constant);
}

static void initCompiler(parser_t *parser, compiler_t *compiler)
{
    compiler->localCount = 0;
    compiler->scopeDepth = 0;
    parser->compiler = compiler;
}

static void endCompiler(parser_t *parser)
{
    emitReturn(parser);

#ifdef DEBUG_PRINT_CODE                      
    if (!parser->hadError) {
        //disassembleChunk(currentChunk(parser), "code");
    }
#endif
}

static void beginScope(parser_t *parser)
{
    compiler_t *current = parser->compiler;
    current->scopeDepth++;
}

static void endScope(parser_t *parser)
{
    compiler_t *current = parser->compiler;
    current->scopeDepth--;

    while (current->localCount > 0 &&
        current->locals[current->localCount - 1].depth >
        current->scopeDepth) {
        emitByte(parser, OP_POP);
        current->localCount--;
    }
}

static void expression(parser_t *parser);
static void statement(parser_t *parser);
static void declaration(parser_t *parser);
static rule_t *getRule(toktype_t type);
static void parsePrecedence(parser_t *parser, prec_t precedence);

static uint16_t identifierConstant(parser_t *parser, tok_t *name)
{
    str_t *id = str_copy(parser->vm, name->start, name->length);
    return makeConstant(parser, VAL_OBJ(id));
}

static bool identifiersEqual(tok_t *a, tok_t *b)
{
    if (a->length != b->length) return false;
    return memcmp(a->start, b->start, a->length) == 0;
}

static int resolveLocal(parser_t *parser, compiler_t *compiler, tok_t *name)
{
    for (int i = compiler->localCount - 1; i >= 0; i--) {
        local_t *local = &compiler->locals[i];
        if (identifiersEqual(name, &local->name)) {
            if (local->depth == -1) {
                error(parser, "Cannot read local variable in its own initializer.");
            }
            return i;
        }
    }

    return -1;
}

static void addLocal(parser_t *parser, tok_t name)
{
    compiler_t *current = parser->compiler;

    if (current->localCount == UINT8_COUNT) {
        error(parser, "Too many local variables in function.");
        return;
    }

    local_t *local = &current->locals[current->localCount++];
    local->name = name;
    local->depth = -1;
}

static void declareVariable(parser_t *parser)
{
    compiler_t *current = parser->compiler;

    // Global variables are implicitly declared.
    if (current->scopeDepth == 0) return;

    tok_t *name = &parser->previous;
    for (int i = current->localCount - 1; i >= 0; i--) {
        local_t *local = &current->locals[i];
        if (local->depth != -1 && local->depth < current->scopeDepth) {
            break;
        }

        if (identifiersEqual(name, &local->name)) {
            error(parser, "Variable with this name already declared in this scope.");
        }
    }

    addLocal(parser, *name);
}

static uint16_t parseVariable(parser_t *parser, const char *errorMessage)
{
    consume(parser, TOKEN_IDENTIFIER, errorMessage);

    declareVariable(parser);
    if (parser->compiler->scopeDepth > 0) return 0;

    return identifierConstant(parser, &parser->previous);
}

static void markInitialized(parser_t *parser)
{
    compiler_t *current = parser->compiler;
    current->locals[current->localCount - 1].depth =
        current->scopeDepth;
}

static void defineVariable(parser_t *parser, uint16_t global)
{
    if (parser->compiler->scopeDepth > 0) {
        markInitialized(parser);
        return;
    }

    emitBytesAndConstLong(parser, OP_DEF, global);
}

static void binary(parser_t *parser, bool canAssign)
{
    // Remember the operator.                                
    toktype_t operatorType = parser->previous.type;

    // Compile the right operand.                            
    rule_t *rule = getRule(operatorType);
    parsePrecedence(parser, (prec_t)(rule->precedence + 1));

    // Emit the operator instruction.                        
    switch (operatorType) {
        case TOKEN_EQUAL_EQUAL:   emitByte(parser, OP_EQ); break;
        case TOKEN_LESS:          emitByte(parser, OP_LT); break;
        case TOKEN_LESS_EQUAL:    emitByte(parser, OP_LE); break;

        case TOKEN_BANG_EQUAL:    emitBytes(parser, OP_EQ, OP_NOT); break;
        case TOKEN_GREATER:       emitBytes(parser, OP_LE, OP_NOT); break;
        case TOKEN_GREATER_EQUAL: emitBytes(parser, OP_LT, OP_NOT); break;

        case TOKEN_PLUS:          emitByte(parser, OP_ADD); break;
        case TOKEN_MINUS:         emitByte(parser, OP_SUB); break;
        case TOKEN_STAR:          emitByte(parser, OP_MUL); break;
        case TOKEN_SLASH:         emitByte(parser, OP_DIV); break;
        default:
            return; // Unreachable.                              
    }
}

static void literal(parser_t *parser, bool canAssign)
{
    switch (parser->previous.type) {
        case TOKEN_FALSE:   emitByte(parser, OP_FALSE); break;
        case TOKEN_NIL:     emitByte(parser, OP_NIL); break;
        case TOKEN_TRUE:    emitByte(parser, OP_TRUE); break;
        default:
            return; // Unreachable.                   
    }
}

static void grouping(parser_t *parser, bool canAssign)
{
    expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(parser_t *parser, bool canAssign)
{
    double n = strtod(parser->previous.start, NULL);
    emitConstant(parser, VAL_NUM(n));
}

static void string(parser_t *parser, bool canAssign)
{
    str_t *s = str_copy(parser->vm,
        parser->previous.start + 1, parser->previous.length - 2);

    emitConstant(parser, VAL_OBJ(s));
}

static void namedVariable(parser_t *parser, tok_t name, bool canAssign)
{
    uint8_t getOp, setOp;
    int arg = resolveLocal(parser, parser->compiler, &name);

    if (arg != -1) {
        getOp = OP_LD;
        setOp = OP_ST;
    }
    else {
        arg = identifierConstant(parser, &name);
        getOp = OP_GLD;
        setOp = OP_GST;
    }

    if (canAssign && match(parser, TOKEN_EQUAL)) {
        expression(parser);
        emitBytesAndConstLong(parser, setOp, arg);
    }
    else {
        emitBytesAndConstLong(parser, getOp, arg);
    }
}

static void variable(parser_t *parser, bool canAssign)
{
    namedVariable(parser, parser->previous, canAssign);
}

static void unary(parser_t *parser, bool canAssign)
{
    toktype_t operatorType = parser->previous.type;

    // Compile the operand.                        
    parsePrecedence(parser, PREC_UNARY);

    // Emit the operator instruction.              
    switch (operatorType) {
        case TOKEN_BANG:    emitByte(parser, OP_NOT); break;
        case TOKEN_MINUS:   emitByte(parser, OP_NEG); break;
        default:
            return; // Unreachable.                    
    }
}

static rule_t rules[] = {
    { grouping, NULL,    PREC_NONE },       // TOKEN_LEFT_PAREN      
    { NULL,     NULL,    PREC_NONE },       // TOKEN_RIGHT_PAREN     
    { NULL,     NULL,    PREC_NONE },       // TOKEN_LEFT_BRACE
    { NULL,     NULL,    PREC_NONE },       // TOKEN_RIGHT_BRACE

    { NULL,     NULL,    PREC_NONE },       // TOKEN_COMMA           
    { NULL,     NULL,    PREC_NONE },       // TOKEN_DOT

    { unary,    binary,  PREC_TERM },       // TOKEN_MINUS           
    { NULL,     binary,  PREC_TERM },       // TOKEN_PLUS            
    { NULL,     NULL,    PREC_NONE },       // TOKEN_SEMICOLON       
    { NULL,     binary,  PREC_FACTOR },     // TOKEN_SLASH           
    { NULL,     binary,  PREC_FACTOR },     // TOKEN_STAR

    { unary,    NULL,    PREC_NONE },       // TOKEN_BANG
    { NULL,     binary,  PREC_EQUALITY },   // TOKEN_BANG_EQUAL
    { NULL,     NULL,    PREC_NONE },       // TOKEN_EQUAL           
    { NULL,     binary,  PREC_EQUALITY },   // TOKEN_EQUAL_EQUAL  
    { NULL,     binary,  PREC_COMPARISON }, // TOKEN_GREATER      
    { NULL,     binary,  PREC_COMPARISON }, // TOKEN_GREATER_EQUAL
    { NULL,     binary,  PREC_COMPARISON }, // TOKEN_LESS         
    { NULL,     binary,  PREC_COMPARISON }, // TOKEN_LESS_EQUAL 

    { variable, NULL,    PREC_NONE },       // TOKEN_IDENTIFIER
    { string,   NULL,    PREC_NONE },       // TOKEN_STRING          
    { number,   NULL,    PREC_NONE },       // TOKEN_NUMBER

    { NULL,     NULL,    PREC_NONE },       // TOKEN_AND             
    { NULL,     NULL,    PREC_NONE },       // TOKEN_CLASS           
    { NULL,     NULL,    PREC_NONE },       // TOKEN_ELSE            
    { literal,  NULL,    PREC_NONE },       // TOKEN_FALSE           
    { NULL,     NULL,    PREC_NONE },       // TOKEN_FOR             
    { NULL,     NULL,    PREC_NONE },       // TOKEN_FUN             
    { NULL,     NULL,    PREC_NONE },       // TOKEN_IF              
    { literal,  NULL,    PREC_NONE },       // TOKEN_NIL             
    { NULL,     NULL,    PREC_NONE },       // TOKEN_OR              
    { NULL,     NULL,    PREC_NONE },       // TOKEN_PRINT           
    { NULL,     NULL,    PREC_NONE },       // TOKEN_RETURN          
    { NULL,     NULL,    PREC_NONE },       // TOKEN_SUPER           
    { NULL,     NULL,    PREC_NONE },       // TOKEN_THIS            
    { literal,  NULL,    PREC_NONE },       // TOKEN_TRUE            
    { NULL,     NULL,    PREC_NONE },       // TOKEN_VAR             
    { NULL,     NULL,    PREC_NONE },       // TOKEN_WHILE

    { NULL,     NULL,    PREC_NONE },       // TOKEN_ERROR           
    { NULL,     NULL,    PREC_NONE },       // TOKEN_EOF             
};

static void parsePrecedence(parser_t *parser, prec_t precedence)
{
    advance(parser);
    parsefn_t prefixRule = getRule(parser->previous.type)->prefix;
    if (prefixRule == NULL) {
        error(parser, "Expect expression.");
        return;
    }

    bool canAssign = precedence <= PREC_ASSIGNMENT;
    prefixRule(parser, canAssign);

    while (precedence <= getRule(parser->current.type)->precedence) {
        advance(parser);
        parsefn_t infixRule = getRule(parser->previous.type)->infix;
        infixRule(parser, canAssign);
    }

    if (canAssign && match(parser, TOKEN_EQUAL)) {
        error(parser, "Invalid assignment target.");
    }
}

static rule_t *getRule(toktype_t type)
{
    return &rules[type];
}

static void expression(parser_t *parser)
{
    parsePrecedence(parser, PREC_ASSIGNMENT);
}

static void block(parser_t *parser)
{
    while (!check(parser, TOKEN_RIGHT_BRACE) && !check(parser, TOKEN_EOF)) {
        declaration(parser);
    }

    consume(parser, TOKEN_RIGHT_BRACE, "Expect '}' after block.");
}

static void varDeclaration(parser_t *parser)
{
    uint16_t global = parseVariable(parser, "Expect variable name.");

    if (match(parser, TOKEN_EQUAL)) {
        expression(parser);
    }
    else {
        emitByte(parser, OP_NIL);
    }
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after variable declaration.");

    defineVariable(parser, global);
}

static void expressionStatement(parser_t *parser)
{
    expression(parser);
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after expression.");

    emitByte(parser, OP_POP);
}

static void printStatement(parser_t *parser)
{
    expression(parser);
    consume(parser, TOKEN_SEMICOLON, "Expect ';' after value.");

    emitByte(parser, OP_PRINT);
}

static void synchronize(parser_t *parser)
{
    parser->panicMode = false;

    while (parser->current.type != TOKEN_EOF) {
        if (parser->previous.type == TOKEN_SEMICOLON) return;

        switch (parser->current.type) {
            case TOKEN_CLASS:
            case TOKEN_FUN:
            case TOKEN_VAR:
            case TOKEN_FOR:
            case TOKEN_IF:
            case TOKEN_WHILE:
            case TOKEN_PRINT:
            case TOKEN_RETURN:
                return;
            default:; // Do nothing.
        }

        advance(parser);
    }
}

static void declaration(parser_t *parser)
{
    if (match(parser, TOKEN_VAR)) {
        varDeclaration(parser);
    }
    else {
        statement(parser);
    }

    if (parser->panicMode) synchronize(parser);
}

static void statement(parser_t *parser)
{
    if (match(parser, TOKEN_PRINT)) {
        printStatement(parser);
    }
    else if (match(parser, TOKEN_LEFT_BRACE)) {
        beginScope(parser);
        block(parser);
        endScope(parser);
    }
    else {
        expressionStatement(parser);
    }
}

bool compile(vm_t *vm, const char *fname, const char *source, chunk_t *chunk)
{
    lexer_t lexer;
    parser_t parser;
    compiler_t compiler;

    lexer_init(&lexer, vm, fname, source);
    initCompiler(&parser, &compiler);
    parser.vm = vm;
    parser.compilingChunk = chunk;
    parser.lexer = &lexer;
    parser.hadError = false;
    parser.panicMode = false;
    
    advance(&parser);
    while (!match(&parser, TOKEN_EOF)) {
        declaration(&parser);
    }

    endCompiler(&parser);
    return !parser.hadError;
}
