
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "glsl_ext.h"

struct sbuf {
    char* buf;
    size_t at; /* index of final null character */
    size_t bsize; /* actual buffer size */
};

#define append(sbuf, str) n_append(sbuf, strlen(str), str)

static inline void expand_for(struct sbuf* sbuf, size_t len) {
    bool resize = false;
    while (len + 1 > sbuf->bsize - sbuf->at) {
        sbuf->bsize *= 2;
        resize = true;
    }
    if (resize)
        sbuf->buf = realloc(sbuf->buf, sbuf->bsize);
}

/* append 'n' bytes from 'str' to the resizable buffer */
static void n_append(struct sbuf* sbuf, size_t len, const char* str) {
    expand_for(sbuf, len);
    memcpy(sbuf->buf + sbuf->at, str, len);
    sbuf->at += len;
    sbuf->buf[sbuf->at] = '\0';
}

#define s_append(sbuf, fmt, ...) se_append(sbuf, 64, fmt, __VA_ARGS__) 

/* append the formatted string to the resizable buffer, where elen is extra space for formatted chars */
static void se_append(struct sbuf* sbuf, size_t elen, const char* fmt, ...) {
    size_t space = strlen(fmt) + elen;
    expand_for(sbuf, space);
    va_list args;
    va_start(args, fmt);
    int written;
    if ((written = vsnprintf(sbuf->buf + sbuf->at, space, fmt, args)) < 0)
        abort();
    sbuf->at += space;
    va_end(args);
}

#define parse_error(line, f, fmt, ...)                                   \
    fprintf(stderr, "[%s:%d] " fmt "\n", f, (int) line, __VA_ARGS__);   \
    abort()

#define parse_error_s(line, f, s)                                  \
    fprintf(stderr, "[%s:%d] " s "\n", f, (int) line);   \
    abort()

struct schar {
    char* buf;
    size_t sz;
};

/* handle raw arguments for #include and #request directives */
/* NOTE: munmap needs to be called on the result */
static struct schar directive(struct glsl_ext* ext, char** args,
                             size_t args_sz, bool include,
                             size_t line, const char* f) {
    if (include) {
        if (args_sz == 0) {
            parse_error_s(line, f, "No arguments provided to #include directive!");
        }
        char* target = args[0];

        /* Handle `:` config specifier */
        size_t tsz = strlen(target);
        if (tsz && target[0] == ':' && ext->cfd) {
            target = &target[1];
            ext->cd = ext->cfd;
        }
        
        char path[strlen(ext->cd) + tsz + 2];
        snprintf(path, sizeof(path) / sizeof(char), "%s/%s", ext->cd, target);
        
        int fd = open(path, O_RDONLY);
        if (fd == -1) {
            parse_error(line, f, "failed to load GLSL shader source specified by #include directive '%s': %s\n",
                        path, strerror(errno));
        }
        
        struct stat st;
        fstat(fd, &st);
        
        char* map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (!map) {
            parse_error(line, f, "failed to map GLSL shader source specified by #include directive '%s': %s\n",
                        path, strerror(errno));
        }

        struct glsl_ext next = {
            .source     = map,
            .source_len = st.st_size,
            .cd         = ext->cd,
            .cfd        = ext->cfd,
            .handlers   = ext->handlers
        };

        /* recursively process */
        ext_process(&next, target);
        
        struct schar ret = {
            .buf = next.processed,
            .sz  = next.p_len
        };

        return ret;
    } else {

        if (args_sz > 0) {
            char* request = args[0];

            struct request_handler* handler;
            bool found = false;
            size_t t, i;
            for (t = 0; (handler = &ext->handlers[t])->name != NULL; ++t) {
                if(!strcmp(handler->name, request)) {
                    found = true;
                    void** processed_args = malloc(strlen(handler->fmt) * sizeof(void*));
                    
                    char c;
                    size_t i;
                    for (i = 0; (c = handler->fmt[i]) != '\0'; ++i) {
                        if (args_sz <= 1 + i) {
                            parse_error(line, f,
                                        "failed to execute request '%s': expected format '%s'\n",
                                        request, handler->fmt);
                        }
                        char* raw = args[1 + i];
                        switch (c) {
                        case 'i':
                            {
                                int v = (int) strtol(raw, NULL, 0);
                                processed_args[i] = malloc(sizeof(int));
                                *(int*) processed_args[i] = v;
                                break;
                            }
                        case 'f':
                            {
                                float f = strtof(raw, NULL);
                                processed_args[i] = malloc(sizeof(float));
                                *(float*) processed_args[i] = f;
                                break;
                            }
                        case 's': { *(char**) &processed_args[i] = raw; break; }
                        case 'b':
                            {
                                bool v;
                                if (!strcmp(raw, "true")) {
                                    v = true;
                                } else if (!strcmp(raw, "false")) {
                                    v = false;
                                } else if (strlen(raw) == 1) {
                                    switch (raw[0]) {
                                    case 't': { v = true; break; }
                                    case 'f': { v = false; break; }
                                    case '1': { v = true; break; }
                                    case '0': { v = false; break; }
                                    default:
                                        parse_error_s(line, f, "tried to parse invalid raw string into a boolean");
                                    }
                                } else {
                                    parse_error_s(line, f, "tried to parse invalid raw string into a boolean");
                                }
                                processed_args[i] = malloc(sizeof(bool));
                                *(bool*) processed_args[i] = v;
                                break;
                            }
                        }
                    }

                    handler->handler(request, processed_args);
                    
                    for (i = 0; (c = handler->fmt[i]) != '\0'; ++i)
                        if (c != 's')
                            free(processed_args[i]);
                    free(processed_args);
                }
            }
            if (!found) {
                parse_error(line, f, "unknown request type '%s'", request);
            }
        }
        
        struct schar ret = {
            .buf = NULL,
            .sz  = 0
        };

        return ret;
    }
}

/* state machine parser */
void ext_process(struct glsl_ext* ext, const char* f) {
    
    #define LINE_START 0
    #define IGNORING 1
    #define MACRO 2
    #define REQUEST 3
    #define INCLUDE 4
    
    struct sbuf sbuf = {
        .buf   = malloc(256),
        .at    = 0,
        .bsize = 256
    };

    size_t source_len = ext->source_len;
    size_t t;
    char at;
    int state = LINE_START;
    size_t macro_start_idx, arg_start_idx;
    size_t line = 1;
    bool quoted = false, arg_start;
    char** args = NULL;
    size_t args_sz = 0;
    
    for (t = 0; t <= source_len; ++t) {
        at = source_len == t ? '\0' : ext->source[t];
        if (at == '\n')
            ++line;
        switch (state) {
        case LINE_START: /* processing start of line */
            {
                switch (at) {
                case '#': {
                    macro_start_idx = t;
                    state = MACRO;
                    continue; }
                case ' ':
                case '\t':
                case '\n':
                    goto copy;
                default: {
                    state = IGNORING;
                    goto copy; }
                }
            }
        case IGNORING: /* copying GLSL source or unrelated preprocessor syntax */
            if (at == '\n') {
                state = LINE_START;
            }
            goto copy;
        case MACRO: /* processing start of macro */
            {
                switch (at) {
                case '\n':
                case ' ':
                case '\t':
                case '\0':
                    {
                        /* end parsing directive */
                        if (!strncmp("#request", &ext->source[macro_start_idx], t - macro_start_idx)
                            || !strncmp("#REQUEST", &ext->source[macro_start_idx], t - macro_start_idx)) {
                            state = REQUEST;
                            goto prepare_arg_parse;
                        } else if (!strncmp("#include", &ext->source[macro_start_idx], t - macro_start_idx)
                                   || !strncmp("#INCLUDE", &ext->source[macro_start_idx], t - macro_start_idx)) {
                            state = INCLUDE;
                            goto prepare_arg_parse;
                        } else {
                            n_append(&sbuf, t - macro_start_idx, &ext->source[macro_start_idx]);
                            state = IGNORING;
                            goto copy;
                        }
                    prepare_arg_parse:
                        {
                            arg_start_idx = t + 1;
                            arg_start = true;
                            args = malloc(sizeof(char*));
                            args_sz = 0;
                            *args = NULL;
                        }
                    }
                case 'a' ... 'z':
                case 'A' ... 'Z':
                    continue;
                default:
                    /* invalid char, malformed! */
                    parse_error(line, f, "Unexpected character '%c' while parsing GLSL directive", at);
                }
            }
            /* scope-violating macro to copy the result of the currently parsed argument */
            #define copy_arg(end)                                       \
                do { if (end - arg_start_idx > 0) {                     \
                        ++args_sz;                                      \
                        args = realloc(args, sizeof(char*) * args_sz);  \
                        args[args_sz - 1] = malloc((end - arg_start_idx) + 1); \
                        memcpy(args[args_sz - 1], &ext->source[arg_start_idx], end - arg_start_idx); \
                        args[args_sz - 1][end - arg_start_idx] = '\0';  \
                    } } while (0)
        case REQUEST:
        case INCLUDE:
            {
                switch (at) {
                case ' ':
                case '\t':
                case '\n':
                case '\0':
                    if (!quoted) {
                        /* end arg */
                        copy_arg(t);
                        arg_start = true;
                        arg_start_idx = t + 1;
                    } else arg_start = false;
                    
                    if (at == '\n') {
                        /* end directive */
                        size_t a;
                        struct schar r = directive(ext, args, args_sz, state == INCLUDE, line, f);
                        for (a = 0; a < args_sz; ++a) {
                            free(args[a]);
                        }
                        args_sz = 0;
                        /* if something was returned (ie. included file), paste the results */
                        if (r.buf) {
                            n_append(&sbuf, r.sz, r.buf);
                            append(&sbuf, "\n");
                        }
                        munmap(r.buf, r.sz);
                        state = LINE_START;
                    }
                    break;
                case '"':
                    if (quoted) {
                        /* end arg */
                        copy_arg(t);
                        quoted = false;
                        arg_start = true;
                        arg_start_idx = t + 1;
                    } else if (arg_start) {
                        ++arg_start_idx;
                        quoted = true;
                    } else arg_start = false;
                    break;
                default:
                    arg_start = false;
                }
                
                continue;
            }
        }
    copy:
        if (at != '\0')
            n_append(&sbuf, 1, &at);
    }
    ext->processed = sbuf.buf;
    ext->p_len = sbuf.at;

    if (args)
        free(args);
}
void ext_free(struct glsl_ext* ext) {
    free(ext->processed);
}
