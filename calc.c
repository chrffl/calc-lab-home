// Polina Pavlova 260ADB159
// https://github.com/chrffl/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: Claude (Anthropic), conversation: https://claude.ai/chat/968c9f9f-ca62-4ff8-be0d-128a241819fe ; Claude (Anthropic), second conversation about the last tasks: <link> ; cppreference.com (opendir/readdir, stat, mkdir, strtod, pow)

// AI (Claude): asked to help write the program step by step, grade by grade.
//   Stage "grade 4-5": file reading, tokenizer with positions, + and -,
//   error positions, output file name.
//   Stage "grade 6-7": * and /, division by zero, parentheses.
//   Stage "grade 8 + 10": ** (right-associative), comment lines, floats.
//   Stage "grade 9": full command line, see the comment above main().
//   Generated with Claude, then tested and read through by me.

// mkdir() is POSIX, not plain C17, so we ask the headers to declare it.
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <dirent.h>

// Used in the output file name: <base>_<name>_<lastname>_<id>.txt
#define STUDENT_NAME     "polina"
#define STUDENT_LASTNAME "pavlova"
#define STUDENT_ID       "260ADB159"

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

enum { T_NUM, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_POW,
       T_LPAREN, T_RPAREN, T_END, T_BAD };

// A value is either an integer or a double. Each number keeps its own type
// from the moment it is read ("7" is int, "7.0" is float), and every
// operation looks at the types of its two operands (see arith()).
typedef struct {
    int is_float;
    long long i;
    double f;
} Value;

typedef struct {
    int type;
    size_t start;           // index of the first character of the token
    size_t end;             // index just after the token
    Value val;              // value, if type == T_NUM
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
    t.val.is_float = 0;
    t.val.i = 0;
    t.val.f = 0.0;
    if (i >= line_end) {
        t.type = T_END;
        t.end = i;
        return t;
    }
    char c = src[i];
    if (isdigit((unsigned char)c)) {
        // Accumulate in unsigned: a long float like 123456789012345678901.5
        // would overflow a signed long long (undefined behaviour), unsigned
        // just wraps. For real integers the tests never overflow.
        unsigned long long v = 0;
        while (i < line_end && isdigit((unsigned char)src[i])) {
            v = v * 10 + (unsigned long long)(src[i] - '0');
            i++;
        }
        t.type = T_NUM;
        t.val.i = (long long)v;
        // A float is digits '.' digits. The '.' is taken only if a digit
        // follows it. Otherwise the number ends before the '.', and the
        // parser then sees '.' as an invalid character, so "5." gives the
        // error at the '.', and ".5" too (no digits before it). "1.5e3":
        // the number is 1.5 and then 'e' is an invalid character.
        if (i + 1 < line_end && src[i] == '.' && isdigit((unsigned char)src[i + 1])) {
            i++;
            while (i < line_end && isdigit((unsigned char)src[i]))
                i++;
            t.val.is_float = 1;
            // We already checked the format ourselves, so strtod just
            // converts it (with correct rounding). It can't run further than
            // our token, because a digit-dot-digit number followed by 'e'
            // is an error anyway.
            t.val.f = strtod(src + t.start, NULL);
        }
        t.end = i;
    } else if (c == '*') {
        // "**" must be checked before "*", otherwise 2 ** 3 would look like
        // two '*' operators in a row.
        if (i + 1 < line_end && src[i + 1] == '*') {
            t.type = T_POW;
            t.end = i + 2;
        } else {
            t.type = T_STAR;
        }
    } else if (c == '+') {
        t.type = T_PLUS;
    } else if (c == '-') {
        t.type = T_MINUS;
    } else if (c == '/') {
        t.type = T_SLASH;
    } else if (c == '(') {
        t.type = T_LPAREN;
    } else if (c == ')') {
        t.type = T_RPAREN;
    } else {
        t.type = T_BAD;     // any other character (also '#', '.') is invalid
    }
    return t;
}

static double as_double(Value v) {
    return v.is_float ? v.f : (double)v.i;
}

// Integer power by squaring: O(log e) multiplications, so 2 ** 1000000000
// style exponents (with base 0, 1 or -1) don't take forever.
// b is squared only if more bits of e are left, so we never compute a power
// bigger than the result itself.
static long long ipow(long long b, long long e) {
    long long r = 1;        // also makes x ** 0 == 1, including 0 ** 0
    while (e > 0) {
        if (e & 1)
            r *= b;
        e >>= 1;
        if (e)
            b *= b;
    }
    return r;
}

// Applies one binary operator.
// Integer vs float: if BOTH operands are integers the operation is done in
// long long (so 7 / 2 = 3, truncated like C). If at least one is a float,
// both are converted to double (so 7 / 2.0 = 3.5). The result type follows
// the same rule.
// op_pos      - index of the operator: a float result that is inf or NaN is
//               reported there, because it's the operator that "failed".
// right_start - index where the right operand starts: division by zero and
//               a negative integer exponent are reported there, because the
//               problem is the value of that operand.
static Value arith(int op, Value a, Value b, size_t op_pos, size_t right_start) {
    Value res = {0, 0, 0.0};
    if (!a.is_float && !b.is_float) {
        long long x = a.i, y = b.i;
        switch (op) {
        case T_PLUS:  res.i = x + y; break;
        case T_MINUS: res.i = x - y; break;
        case T_STAR:  res.i = x * y; break;
        case T_SLASH:
            if (y == 0) { fail(right_start); return res; }
            res.i = x / y;
            break;
        case T_POW:
            if (y < 0) { fail(right_start); return res; }
            res.i = ipow(x, y);
            break;
        }
        return res;
    }
    double x = as_double(a), y = as_double(b);
    res.is_float = 1;
    switch (op) {
    case T_PLUS:  res.f = x + y; break;
    case T_MINUS: res.f = x - y; break;
    case T_STAR:  res.f = x * y; break;
    case T_SLASH:
        if (y == 0.0) { fail(right_start); return res; }
        res.f = x / y;
        break;
    case T_POW:   res.f = pow(x, y); break;   // (0 - 8) ** 0.5 -> NaN
    }
    if (!isfinite(res.f))
        fail(op_pos);
    return res;
}

// ---------------------------------------------------------------------------
// Recursive descent parser, one function per precedence level:
//
//   expr    := term    { ('+' | '-') term }
//   term    := power   { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
//
// Why the precedence is right: each level can only combine whole results of
// the level below it, and those are parsed completely first. In
// 2 * 3 ** 2 + 1, term gets "3 ** 2" as one power before it applies *, and
// expr gets "2 * 9" as one term before it applies +.
// The deeper the function, the stronger the operator binds.
// ---------------------------------------------------------------------------
static Value parse_expr(void);

// If we expected a number or '(' but see something else (an operator, ')',
// a bad character or the end of the line), the error is at that token,
// e.g. "2 * ()" -> the ')' at position 6. This also makes unary minus an
// error: in "-5" the parser expects a number and finds '-'.
static Value parse_primary(void) {
    Value zero = {0, 0, 0.0};
    Token t = peek();
    if (t.type == T_NUM) {
        cur = t.end;
        return t.val;
    }
    if (t.type == T_LPAREN) {
        cur = t.end;
        Value v = parse_expr();
        if (err_pos)
            return zero;
        // Inside the brackets the expression must end with ')'. If the line
        // ends instead, peek() returns T_END at the '\n', which is exactly
        // where "missing )" must be reported.
        Token r = peek();
        if (r.type != T_RPAREN) {
            fail(r.start);
            return zero;
        }
        cur = r.end;
        return v;
    }
    fail(t.start);
    return zero;
}

// power := primary [ '**' power ]
// power() calls itself for the exponent, so 2 ** 3 ** 2 groups as
// 2 ** (3 ** 2) = 512: the right side is finished before we apply our **.
// That is what makes ** right-associative, unlike the loops in term/expr.
static Value parse_power(void) {
    Value base = parse_primary();
    if (err_pos)
        return base;
    Token op = peek();
    if (op.type != T_POW)
        return base;
    cur = op.end;
    size_t right_start = peek().start;  // start of the exponent, see arith()
    Value exp = parse_power();
    if (err_pos)
        return exp;
    return arith(T_POW, base, exp, op.start, right_start);
}

// term := power { ('*' | '/') power }
// A loop (not recursion) makes * and / left-associative: 100 / 7 / 2 = 7.
static Value parse_term(void) {
    Value v = parse_power();
    while (err_pos == 0) {
        Token op = peek();
        if (op.type != T_STAR && op.type != T_SLASH)
            break;
        cur = op.end;
        // Remember where the right operand starts BEFORE parsing it:
        // division by zero is reported there, e.g. "8 / (4 - 2 * 2)" -> the
        // '(' at position 5, not the last 2 that made it zero.
        size_t right_start = peek().start;
        Value r = parse_power();
        if (err_pos)
            return r;
        v = arith(op.type, v, r, op.start, right_start);
    }
    return v;
}

// expr := term { ('+' | '-') term }
// A loop (not recursion) makes + and - left-associative: 10 - 3 - 2 = 5.
static Value parse_expr(void) {
    Value v = parse_term();
    while (err_pos == 0) {
        Token op = peek();
        if (op.type != T_PLUS && op.type != T_MINUS)
            break;
        cur = op.end;
        size_t right_start = peek().start;
        Value r = parse_term();
        if (err_pos)
            return r;
        v = arith(op.type, v, r, op.start, right_start);
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

        // Skip leading spaces to see what kind of line this is.
        size_t first = i;
        while (first < line_end && (text[first] == ' ' || text[first] == '\t'))
            first++;

        // Blank lines give no output. A line starting with '#' is a comment
        // and is skipped as a whole, but its characters still count for
        // positions, because positions are indexes in the whole file.
        // A '#' anywhere else is not handled here, so the tokenizer reports
        // it as an invalid character.
        if (first < line_end && text[first] != '#') {
            Value v = parse_expr();
            if (err_pos == 0) {
                // After a full expression only the end of line may follow.
                // A ')' here has no matching '(' (expr stopped at it), so
                // "extra )" is reported at that ')'.
                Token t = peek();
                if (t.type != T_END)
                    fail(t.start);
            }
            if (err_pos == 0) {
                char num[64];
                int k = v.is_float ? snprintf(num, sizeof num, "%.15g\n", v.f)
                                   : snprintf(num, sizeof num, "%lld\n", v.i);
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
    // A file with only blank/comment lines gives an empty output; then
    // out.data is still NULL and must not be passed to fwrite.
    int bad = 0;
    if (out.len > 0 && fwrite(out.data, 1, out.len, f) != out.len)
        bad = 1;
    if (fclose(f) != 0)
        bad = 1;
    free(out.data);
    if (bad) {
        fprintf(stderr, "calc: cannot write '%s'\n", path);
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Command line (grade 9).
// AI (Claude, separate chat): prompt "here is all my task for u to understand
//   what I've been doing all that time with other chats: here is also my
//   almost ready repository: https://github.com/chrffl/calc-lab-home right now
//   I am testing my app and I need help with 2 last tasks: tell me what is a
//   better way to do that?" Link: <link>
//   I asked there how to do the last two tasks (the full command line and
//   the remaining grades). The code below was then written with Claude
//   (main conversation, link on line 4) and checked with cli.sh.
// ---------------------------------------------------------------------------

static void usage(void) {
    fprintf(stderr,
            "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
}

// Last part of a path, without a trailing '/' and without ".txt":
// "labs/task1.txt" -> "task1", "../in" -> "in", "labs/" -> "labs".
// Used for the default output folder <input_base>_<name>_<id>.
// The trailing '/' is cut first, otherwise "labs/" would give an empty name.
static void path_base(const char *path, char *dst, size_t size) {
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    size_t start = n;
    while (start > 0 && path[start - 1] != '/')
        start--;
    size_t len = n - start;
    if (len >= 4 && strncmp(path + start + len - 4, ".txt", 4) == 0)
        len -= 4;
    if (len >= size)
        len = size - 1;
    memcpy(dst, path + start, len);
    dst[len] = '\0';
}

// Creates the output folder if needed. An existing folder is fine (we write
// into it), but an existing FILE with that name is an error, because then
// fopen() of every result would fail later with a less clear message.
static int make_outdir(const char *dir) {
    if (mkdir(dir, 0777) == 0)
        return 0;
    struct stat st;
    if (errno == EEXIST && stat(dir, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    fprintf(stderr, "calc: cannot create output folder '%s': %s\n", dir, strerror(errno));
    return 1;
}

static int ends_with_txt(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".txt") == 0;
}

// Evaluates every *.txt file directly inside dir. Subfolders and other
// files are skipped: stat() tells us if an entry is a regular file
// (d_type from readdir is not in POSIX, so we don't rely on it).
// One bad file does not stop the others; the exit code is non-zero then.
static int process_dir(const char *dir, const char *outdir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return 1;
    }
    int rc = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with_txt(e->d_name))
            continue;
        // dir + '/' + name + '\0'; malloc, so there is no length limit
        size_t n = strlen(dir) + strlen(e->d_name) + 2;
        char *path = malloc(n);
        if (!path) {
            fprintf(stderr, "calc: out of memory\n");
            closedir(d);
            return 1;
        }
        snprintf(path, n, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            if (process_file(path, outdir) != 0)
                rc = 1;
        }
        free(path);
    }
    closedir(d);
    return rc;
}

int main(int argc, char **argv) {
    const char *dir = NULL, *outdir = NULL, *input = NULL;

    if (argc < 2) {
        usage();
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0 ||
            strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a folder name\n", a);
                usage();
                return 1;
            }
            if (a[1] == 'd' || strcmp(a, "--dir") == 0)
                dir = argv[++i];
            else
                outdir = argv[++i];
        } else if (a[0] == '-') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 1;
        } else if (input == NULL) {
            input = a;
        } else {
            fprintf(stderr, "calc: only one input file is allowed\n");
            usage();
            return 1;
        }
    }
    if ((dir == NULL) == (input == NULL)) {
        fprintf(stderr, "calc: give either -d DIR or one input file\n");
        usage();
        return 1;
    }

    // Check the input BEFORE creating the output folder, so a typo in the
    // input name doesn't leave an empty folder behind.
    const char *in = dir ? dir : input;
    struct stat st;
    if (stat(in, &st) != 0) {
        fprintf(stderr, "calc: cannot find '%s': %s\n", in, strerror(errno));
        return 1;
    }
    if (dir && !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is not a folder\n", dir);
        return 1;
    }

    // Default output folder: <input_base>_<name>_<id> in the current folder.
    char def_out[1100];
    if (!outdir) {
        char base[1024];
        path_base(in, base, sizeof base);
        snprintf(def_out, sizeof def_out, "%s_%s_%s", base, STUDENT_NAME, STUDENT_ID);
        outdir = def_out;
    }
    if (make_outdir(outdir) != 0)
        return 1;

    // An ERROR:<pos> result is a normal result, so these return 0 for it;
    // they return 1 only if a file can't be read or written.
    return dir ? process_dir(dir, outdir) : process_file(input, outdir);
}