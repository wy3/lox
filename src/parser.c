#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parser.h"
#include "lexer.h"

typedef struct {
    vm_t *vm;
    chunk_t *compilingChunk;
    lexer_t *lexer;
    tok_t current;
    tok_t previous;
    bool hadError;
    bool panicMode;
} parser_t;

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

static void emitConstant(parser_t *parser, val_t value)
{
    uint16_t constant = makeConstant(parser, value);

    if (constant > UINT8_MAX) {
        emitByte(parser, OP_CONSTL);
        emitBytes(parser, (constant >> 8) & 0xFF, constant & 0xFF);
        return;
    }

    emitBytes(parser, OP_CONST, (uint8_t)constant);
}

static void endCompiler(parser_t *parser)
{
    emitReturn(parser);
}

static void grouping(parser_t *parser)
{
    expression(parser);
    consume(parser, TOKEN_RIGHT_PAREN, "Expect ')' after expression.");
}

static void number(parser_t *parser)
{
    double n = strtod(parser->previous.start, NULL);
    emitConstant(parser, VAL_NUM(n));
}

static void unary(parser_t *parser)
{
    toktype_t operatorType = parser->previous.type;

    // Compile the operand.                        
    parsePrecedence(parser, PREC_UNARY);

    // Emit the operator instruction.              
    switch (operatorType) {
        case TOKEN_MINUS: emitByte(parser, OP_NEG); break;
        default:
            return; // Unreachable.                    
    }
}

static void parsePrecedence(parser_t *parser, prec_t precedence)
{
    // What goes here?                                
}

static void expression(parser_t *parser)
{
    parsePrecedence(parser, PREC_ASSIGNMENT);
}

bool compile(vm_t *vm, const char *fname, const char *source, chunk_t *chunk)
{
    lexer_t lexer;
    parser_t parser;

    lexer_init(&lexer, vm, fname, source);
    parser.vm = vm;
    parser.compilingChunk = chunk;
    parser.lexer = &lexer;
    parser.hadError = false;
    parser.panicMode = false;
    
    advance(&parser);
    expression(&parser);
    consume(&parser, TOKEN_EOF, "Expect end of expression.");

    endCompiler(&parser);
    return !parser.hadError;
}