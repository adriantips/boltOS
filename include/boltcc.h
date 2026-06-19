#pragma once

/* ===========================================================================
 *  BoltCC  -  a small, freestanding C-family compiler + bytecode VM.
 *
 *  One real compiler pipeline (lexer -> recursive-descent parser -> AST ->
 *  stack-bytecode codegen -> register/stack VM) with three front-end dialects:
 *  C, C++ and C#. It genuinely compiles the source to bytecode and executes it;
 *  it is not an interpreter over the text. Values are 64-bit integers and
 *  strings (the kernel is built with no FPU, so there is no floating point).
 *
 *  Supported across all three dialects: functions (with recursion and forward
 *  references), int/char/long/bool/string/var declarations, arithmetic, bitwise
 *  and comparison operators, &&/||/! with short-circuit, ?: , ++/-- (pre+post),
 *  compound assignment, if/else, while, for, return, break, continue, blocks,
 *  string concatenation with +, string indexing, and a small builtin library
 *  (len, str, int, abs, min, max, chr, ord, pow, sqrt, gcd, upper, lower,
 *  substr). Per dialect:
 *    C   : printf (real %d/%i/%u/%x/%c/%s/%% formatting), puts, putchar
 *    C++ : std::cout << ... << std::endl  (chained), printf, classes ignored
 *    C#  : Console.WriteLine / Console.Write  (incl. {0} {1} format), class +
 *          namespace + using are unwrapped, entry point is Main()
 *  Output is written through kputc(), so the IDE captures it with
 *  console_set_sink(), exactly like BoltPython.
 * ===========================================================================*/

enum { BCC_C = 0, BCC_CPP = 1, BCC_CSHARP = 2 };

const char *boltcc_lang_name(int lang);
const char *boltcc_version(void);

/* Compile and run src for the given dialect. Returns 0 on success; nonzero on a
 * compile or runtime error (a diagnostic has already been printed). */
int  boltcc_run(int lang, const char *src);
