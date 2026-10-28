//Polina Pavlova 260ADB159
//https://github.com/chrffl/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
//Resources used: Claude, conversation: https://claude.ai/chat/968c9f9f-ca62-4ff8-be0d-128a241819fe     cppreference.com

// AI (Claude): asked to help write the program step by step, grade by grade.
//   Stage "grade 4-5": file reading, tokenizer with positions, + and -,
//   error positions, output file name.
//   Stage "grade 6-7": * and /, division by zero, parentheses.
//   Generated with Claude, then tested and read through by me.
 
// mkdir() is POSIX, not plain C17, so we ask the headers to declare it.
#define _POSIX_C_SOURCE 200809L
 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
 
// Used in the output file name: <base>_<name>_<lastname>_<id>.txt
#define STUDENT_NAME     "diana"
#define STUDENT_LASTNAME "frunza"
#define STUDENT_ID       "000000"
 
// ---------------------------------------------------------------------------
// Parser state.
// The whole file is kept in one buffer (src), and the parser works with
// indexes into it. Because of that, the index of a character IS its position
// in the file (0-based), so an error position is simply index + 1. Comment
// lines and '\n' are counted automatically, nothing extra has to be tracked.
// ---------------------------------------------------------------------------
static const char *src;     // whole input file
static size_t cur;          // index of the next unread character
static size_t line_end;     // index of the '\n' of the current line (or file length)
static size_t err_pos;      // 1-based position of the first error, 0 = no error
 
enum { T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_LPAREN, T_RPAREN, T_END, T_BAD };
 
typedef struct {
    int type;
    size_t start;           // index of the first character of the token
    size_t end;             // index just after the token
    long long ival;         // value, if type == T_NUM
} Token;
 
// Remember only the FIRST error; later ones are ignored.
static void fail(size_t index) {
    if (err_pos == 0)
        err_pos = index + 1;
}
 
// Looks at the next token without consuming it (the caller sets cur = t.end).
// The parser only tokenizes what it actually needs, so an error is always
// reported at the token the parser was looking at, as the rules require.
// At the end of a line the token is T_END with start = line_end, which is the
// index of '\n' (or the file length), so "missing operand at end of line"
// gets the position of '\n' / file length + 1 for free.
static Token peek(void) {
    Token t;
    size_t i = cur;
    while (i < line_end && (src[i] == ' ' || src[i] == '\t'))
        i++;
    t.start = i;
    t.end = i + 1;
    t.ival = 0;
    if (i >= line_end) {
        t.type = T_END;
        t.end = i;
        return t;
    }
    char c = src[i];
    if (isdigit((unsigned char)c)) {
        long long v = 0;
        while (i < line_end && isdigit((unsigned char)src[i])) {
            v = v * 10 + (src[i] - '0');
            i++;
        }
        t.type = T_NUM;
        t.ival = v;
        t.end = i;
    } else if (c == '+') {
        t.type = T_PLUS;
    } else if (c == '-') {
        t.type = T_MINUS;
    } else if (c == '*') {
        t.type = T_STAR;
    } else if (c == '/') {
        t.type = T_SLASH;
    } else if (c == '(') {
        t.type = T_LPAREN;
    } else if (c == ')') {
        t.type = T_RPAREN;
    } else {
        t.type = T_BAD;     // any other character is invalid
    }
    return t;
}
 
// ---------------------------------------------------------------------------
// Recursive descent parser, one function per precedence level:
//
//   expr    := term    { ('+' | '-') term }
//   term    := primary { ('*' | '/') primary }
//   primary := NUMBER | '(' expr ')'
//
// Why the precedence is right: expr can only combine whole terms, and a term
// is parsed completely (all its * and /) before expr sees the next + or -.
// So in 3 + 2 * 5, "2 * 5" is already one term when + is applied.
// The deeper the function, the stronger the operator binds.
// ---------------------------------------------------------------------------
static long long parse_expr(void);
 
// If we expected a number or '(' but see something else (an operator, ')',
// a bad character or the end of the line), the error is at that token,
// e.g. "2 * ()" -> the ')' at position 6.
static long long parse_primary(void) {
    Token t = peek();
    if (t.type == T_NUM) {
        cur = t.end;
        return t.ival;
    }
    if (t.type == T_LPAREN) {
        cur = t.end;
        long long v = parse_expr();
        if (err_pos)
            return 0;
        // Inside the brackets the expression must end with ')'. If the line
        // ends instead, peek() returns T_END at the '\n', which is exactly
        // where "missing )" must be reported.
        Token r = peek();
        if (r.type != T_RPAREN) {
            fail(r.start);
            return 0;
        }
        cur = r.end;
        return v;
    }
    fail(t.start);
    return 0;
}
 
// term := primary { ('*' | '/') primary }
// A loop (not recursion) makes * and / left-associative: 100 / 7 / 2 = 7.
static long long parse_term(void) {
    long long v = parse_primary();
    while (err_pos == 0) {
        Token op = peek();
        if (op.type != T_STAR && op.type != T_SLASH)
            break;
        cur = op.end;
        // Remember where the right operand starts BEFORE parsing it:
        // division by zero is reported there, e.g. "8 / (4 - 2 * 2)" -> the
        // '(' at position 5, not the last 2 that made it zero.
        size_t right_start = peek().start;
        long long r = parse_primary();
        if (err_pos)
            return 0;
        if (op.type == T_STAR) {
            v = v * r;
        } else {
            if (r == 0) {
                fail(right_start);
                return 0;
            }
            v = v / r;      // C truncates toward zero, as the rules want
        }
    }
    return v;
}
 
// expr := term { ('+' | '-') term }
// A loop (not recursion) makes + and - left-associative: 10 - 3 - 2 = 5.
static long long parse_expr(void) {
    long long v = parse_term();
    while (err_pos == 0) {
        Token op = peek();
        if (op.type != T_PLUS && op.type != T_MINUS)
            break;
        cur = op.end;
        long long r = parse_term();
        if (err_pos)
            return 0;
        v = (op.type == T_PLUS) ? v + r : v - r;
    }
    return v;
}
 
// ---------------------------------------------------------------------------
// Growing output buffer. Results are collected in memory first because if
// any line has an error, the file must contain only ERROR:<pos>.
// ---------------------------------------------------------------------------
typedef struct {
    char *data;
    size_t len, cap;
} Buffer;
 
static void buf_append(Buffer *b, const char *s, size_t n) {
    if (b->len + n > b->cap) {
        // Double the capacity, so appending N bytes costs O(N) in total
        // instead of copying the whole buffer on every append.
        size_t cap = b->cap ? b->cap : 4096;
        while (b->len + n > cap)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) {
            fprintf(stderr, "calc: out of memory\n");
            exit(1);
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
}
 
// Reads the whole file into memory with one growing buffer (doubling, see
// buf_append). Returns NULL if the file can't be opened.
static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    Buffer b = {0};
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        buf_append(&b, chunk, n);
    fclose(f);
    buf_append(&b, "", 1);      // terminating '\0', not counted in length
    *out_len = b.len - 1;
    return b.data;
}
 
// Evaluates all lines of the file and returns the text for the output file.
static void evaluate(const char *text, size_t len, Buffer *out) {
    src = text;
    err_pos = 0;
    size_t i = 0;
    while (i < len && err_pos == 0) {
        // memchr is much faster than a hand-written loop on big files
        const char *nl = memchr(text + i, '\n', len - i);
        line_end = nl ? (size_t)(nl - text) : len;
        cur = i;
 
        if (peek().type != T_END) {         // blank lines give no output
            long long v = parse_expr();
            if (err_pos == 0) {
                // After a full expression only the end of line may follow.
                // A ')' here has no matching '(' (expr stopped at it), so
                // "extra )" is reported at that ')'.
                Token t = peek();
                if (t.type != T_END)
                    fail(t.start);
            }
            if (err_pos == 0) {
                char num[32];
                int k = snprintf(num, sizeof num, "%lld\n", v);
                buf_append(out, num, (size_t)k);
            }
        }
        i = line_end + 1;
    }
    if (err_pos) {
        // Throw away the partial results, only the first error is written.
        char msg[48];
        int k = snprintf(msg, sizeof msg, "ERROR:%zu\n", err_pos);
        out->len = 0;
        buf_append(out, msg, (size_t)k);
    }
}
 
// "labs/task1.txt" -> "task1"
static void base_name(const char *path, char *dst, size_t size) {
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    size_t n = strlen(name);
    if (n >= 4 && strcmp(name + n - 4, ".txt") == 0)
        n -= 4;
    if (n >= size)
        n = size - 1;
    memcpy(dst, name, n);
    dst[n] = '\0';
}
 
// Evaluates one input file and writes the result into outdir.
// Returns 0 on success, 1 on error (message already printed).
static int process_file(const char *in_path, const char *outdir) {
    size_t len;
    char *text = read_file(in_path, &len);
    if (!text) {
        fprintf(stderr, "calc: cannot read '%s': %s\n", in_path, strerror(errno));
        return 1;
    }
    Buffer out = {0};
    evaluate(text, len, &out);
    free(text);
 
    char base[1024], path[4096];
    base_name(in_path, base, sizeof base);
    snprintf(path, sizeof path, "%s/%s_%s_%s_%s.txt",
             outdir, base, STUDENT_NAME, STUDENT_LASTNAME, STUDENT_ID);
 
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", path, strerror(errno));
        free(out.data);
        return 1;
    }
    fwrite(out.data, 1, out.len, f);
    int bad = (fclose(f) != 0);
    free(out.data);
    if (bad) {
        fprintf(stderr, "calc: cannot write '%s'\n", path);
        return 1;
    }
    return 0;
}
 
// For now only "calc -o OUTDIR input.txt" (the full command line is grade 9).
int main(int argc, char **argv) {
    const char *outdir = NULL, *input = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outdir = argv[++i];
        else
            input = argv[i];
    }
    if (!outdir || !input) {
        fprintf(stderr, "usage: calc -o OUTDIR input.txt\n");
        return 1;
    }
    // Create the folder; if it already exists that's fine too.
    if (mkdir(outdir, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create '%s': %s\n", outdir, strerror(errno));
        return 1;
    }
    return process_file(input, outdir);
}