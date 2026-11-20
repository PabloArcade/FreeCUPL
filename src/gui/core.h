/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecuplw - the parts that do not need Windows: tool messages,
   options and command lines, the settings file, source scanning.
   DARKSOFT. */
#ifndef FREECUPLW_CORE_H
#define FREECUPLW_CORE_H
#include <stddef.h>

/* ---- device table (build/gen/guidev.c, from the .dev files) ---- */
typedef struct {
    const char *mn;        /* mnemonic */
    int pins;
    const char *part;      /* part name */
} GuiDev;
extern const GuiDev gui_devs[];
extern const int gui_ndevs;

/* every word of filter occurs in the mnemonic or the part name */
int gui_dev_match(const char *filter, const GuiDev *d);
/* Order of two devices in a list sorted by column col: 0 mnemonic,
   1 pins, 2 part.  Text compares without case and with runs of digits
   by their value (g16v8 before g20v8, p8 before p10); pins compare as
   numbers; equal keys fall back to the mnemonic. */
int gui_dev_cmp(const GuiDev *a, const GuiDev *b, int col);
/* The devices that match filter, as indices into gui_devs: in table
   order when col < 0, else sorted by col, downwards when desc.  Returns
   the count; idx has room for gui_ndevs entries. */
int gui_dev_list(const char *filter, int col, int desc, int *idx);

/* ---- tool messages ---- */
enum {
    GM_TEXT = 0,   /* anything else */
    GM_NOTE,       /* [nnnnxx] Please note: ... */
    GM_WARN,       /* [nnnnxx] Warning: ... */
    GM_ERROR,      /* [nnnnxx] ... */
    GM_CONTEXT,    /* " 25:source text" before a syntax error */
    GM_ABORT,      /* "Fatal errors encountered", "Simulation errors" */
    GM_FILE,       /* set by the caller: an output file */
    GM_CMD,        /* set by the caller: the command line */
    GM_SUMMARY     /* set by the caller */
};
typedef struct {
    int kind;
    int line;              /* source line, 0 when none */
    char code[8];          /* "0011cb", "" when none */
} GuiMsg;
void gui_parse_msg(const char *text, GuiMsg *m);
/* "Compile: OK - 0 errors, 1 warning" */
void gui_summary(char *out, size_t n, const char *what, int nerr, int nwarn, int rc);

/* ---- options ---- */
typedef struct {
    int jed, doc, fuseplot, lst, abs, mx;   /* -j -x -f -l -a -e */
    int namesrc, secure, kb;                /* -n -g -kb */
    int min;                                /* -m0..-m4 */
    int simafter;                           /* -s on compile */
    int compat;                             /* -compat-refdefect */
    int simlatch;                           /* -simlatch */
    int simlst, simjed;                     /* freecsim -l -j */
    char device[32];                        /* "" = from the source */
} GuiOpts;
void gui_opts_default(GuiOpts *o);

enum { GC_ABS = 1, GC_SIM = 2 };
/* Command lines; exe and file are quoted as needed.  Return 0, or -1
   when out is too small. */
int gui_compile_cmd(const GuiOpts *o, const char *exe, const char *file,
                    int flags, char *out, size_t n);
int gui_sim_cmd(const GuiOpts *o, const char *exe, const char *file,
                char *out, size_t n);
/* The same as argument vectors: v[0..n-1], NULL after the last.  exe,
   file and o must outlive the vector. */
#define GUI_MAXARGS 10
typedef struct {
    const char *v[GUI_MAXARGS + 1];
    int n;
    char flags[16], min[8];
} GuiArgv;
void gui_compile_argv(const GuiOpts *o, const char *exe, const char *file,
                      int flags, GuiArgv *a);
void gui_sim_argv(const GuiOpts *o, const char *exe, const char *file,
                  GuiArgv *a);
/* the arguments quoted and joined with blanks */
int gui_join_args(const GuiArgv *a, char *out, size_t n);
/* one argument by the rules of the Windows C run-time */
int gui_quote_arg(const char *arg, char *out, size_t n);

/* ---- settings file ---- */
typedef struct GuiIni GuiIni;
GuiIni *gui_ini_load(const char *path);     /* a missing file is empty */
int gui_ini_save(const GuiIni *ini, const char *path);
void gui_ini_free(GuiIni *ini);
const char *gui_ini_get(const GuiIni *ini, const char *sec, const char *key,
                        const char *def);
int gui_ini_int(const GuiIni *ini, const char *sec, const char *key, int def);
void gui_ini_set(GuiIni *ini, const char *sec, const char *key, const char *val);
void gui_ini_set_int(GuiIni *ini, const char *sec, const char *key, int val);
void gui_ini_del_section(GuiIni *ini, const char *sec);
int gui_ini_has_section(const GuiIni *ini, const char *sec);

void gui_opts_load(const GuiIni *ini, const char *sec, GuiOpts *o);
void gui_opts_save(GuiIni *ini, const char *sec, const GuiOpts *o);
/* section holding the options of one source file */
void gui_file_section(const char *path, char *out, size_t n);

/* recent files, newest first; list[i] are n buffers of GUI_PATH bytes */
#define GUI_PATH 260
#define GUI_NRECENT 8
void gui_recent_add(char list[GUI_NRECENT][GUI_PATH], const char *path);

/* ---- source text ---- */
enum { GA_PLAIN = 0, GA_COMMENT, GA_KEYWORD, GA_NUMBER };
/* reserved words in lower case, sorted */
extern const char *const gui_keywords[];
extern const int gui_nkeywords;
/* attr[i] gets the class of text[i] */
void gui_scan(const unsigned char *text, size_t n, unsigned char *attr);
/* common prefix and suffix lengths of two texts (they do not overlap) */
void gui_diff(const unsigned char *a, size_t na, const unsigned char *b,
              size_t nb, size_t *prefix, size_t *suffix);
/* name of the JEDEC file (without extension) that the Name header item
   gives; 0 when there is no item or freecupl refuses it */
int gui_design_name(const char *src, size_t n, char *out, size_t outn);
/* the Device statement, from its first letter to after the ';' */
int gui_device_stmt(const char *src, size_t n, size_t *start, size_t *end);
/* 1 when the text uses LF line ends, 0 for CRLF (and for no line end) */
int gui_detect_lf(const char *buf, size_t n);

/* ---- paths ('\\' and '/') ---- */
const char *gui_basename(const char *path);
/* path with its extension replaced; ext has the dot */
void gui_set_ext(const char *path, const char *ext, char *out, size_t n);
/* directory of path without the trailing separator, "." when none */
void gui_dirname(const char *path, char *out, size_t n);
int gui_has_ext(const char *path, const char *ext);
/* a file the tools write: shown read only */
int gui_is_output(const char *path);
int gui_stricmp(const char *a, const char *b);

#endif
