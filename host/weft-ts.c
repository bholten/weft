/* weft-ts.c -- the `ts` lcl package: tree-sitter parse sessions
 * (plan P4).
 *
 * The host exposes DATA, not policy: a session (parser + highlight
 * query + last tree, one per buffer) and a spans call that parses the
 * given text and returns every passing highlight capture as
 * #{start N end N capture "name"}.  Which captures get which colors,
 * when to rescan, and where annotations live are all Lcl -- the
 * legacy host baked the palette into C, a decision deliberately not
 * ported.
 *
 * Unlike the legacy highlighter, query predicates (#eq?, #any-of?,
 * #match?) ARE evaluated -- the lcl grammar's highlights.scm depends
 * on them to tell keywords from ordinary bare words; without them
 * every bare word matches every pattern.  Unknown predicates and
 * directives (#set!) accept, matching upstream behavior of ignoring
 * what a consumer does not implement.
 *
 * Grammars are compiled in (submodule sources); highlight queries are
 * read from the submodule tree at session open (WEFT_TS_QUERY_DIR is
 * baked absolute, so sessions are CWD-proof).  Parsing is
 * incremental: Ts::edit feeds document deltas to the session's tree
 * (see its contract comment), and spans reparses against it -- only
 * the changed regions re-lex.  The legacy host reused trees WITHOUT
 * the edits and silently misaligned its captures.
 */

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lcl.h>
#include <tree_sitter/api.h>

#ifdef WEFT_TS_LCL
const TSLanguage *tree_sitter_lcl(void);
#endif
#ifdef WEFT_TS_C
const TSLanguage *tree_sitter_c(void);
#endif

typedef const TSLanguage *(*weft_ts_lang_fn)(void);

typedef struct {
  const char *name;
  weft_ts_lang_fn fn;
  const char *query_path;
} weft_ts_lang;

static const weft_ts_lang WEFT_TS_LANGS[] = {
#ifdef WEFT_TS_LCL
    {"lcl", tree_sitter_lcl,
     WEFT_TS_QUERY_DIR "/tree-sitter-lcl/queries/highlights.scm"},
#endif
#ifdef WEFT_TS_C
    {"c", tree_sitter_c, WEFT_TS_QUERY_DIR "/c/queries/highlights.scm"},
#endif
    {NULL, NULL, NULL},
};

#define WEFT_TS_SESSION_TAG "weft_ts_session"

typedef struct {
  TSParser *parser;
  TSQuery *query;
  TSTree *tree;
  char lang[32];
  int closed;
} weft_ts_session;

static void session_release(weft_ts_session *s) {
  if (s->closed) {
    return;
  }
  if (s->tree) {
    ts_tree_delete(s->tree);
    s->tree = NULL;
  }
  if (s->query) {
    ts_query_delete(s->query);
    s->query = NULL;
  }
  if (s->parser) {
    ts_parser_delete(s->parser);
    s->parser = NULL;
  }
  s->closed = 1;
}

static void session_finalize(void *ptr) {
  weft_ts_session *s = ptr;
  session_release(s);
  free(s);
}

static const weft_ts_lang *lang_by_name(const char *name) {
  const weft_ts_lang *l;
  for (l = WEFT_TS_LANGS; l->name; l++) {
    if (strcmp(l->name, name) == 0) {
      return l;
    }
  }
  return NULL;
}

static char *read_whole_file(const char *path) {
  FILE *f = fopen(path, "rb");
  long size;
  char *buf;
  size_t got;

  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  buf = malloc((size_t)size + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  got = fread(buf, 1, (size_t)size, f);
  fclose(f);
  buf[got] = '\0';
  return buf;
}

/* ---- predicate evaluation ---- */

/* Text of the first node captured under `value_id` in this match. */
static int capture_text(const TSQueryMatch *m, uint32_t value_id,
                        const char *text, uint32_t text_len,
                        const char **out, uint32_t *out_len) {
  uint16_t i;
  for (i = 0; i < m->capture_count; i++) {
    if (m->captures[i].index == value_id) {
      uint32_t s = ts_node_start_byte(m->captures[i].node);
      uint32_t e = ts_node_end_byte(m->captures[i].node);
      if (s > text_len || e > text_len || e < s) {
        return 0;
      }
      *out = text + s;
      *out_len = e - s;
      return 1;
    }
  }
  return 0;
}

/* One predicate: steps[0] is the operator string, then arguments,
 * ending before the Done step.  Returns 1 = pass. */
static int eval_predicate(const TSQuery *q, const TSQueryMatch *m,
                          const char *text, uint32_t text_len,
                          const TSQueryPredicateStep *steps, uint32_t n) {
  uint32_t op_len;
  const char *op;
  const char *cap;
  uint32_t cap_len;
  int negate = 0;

  if (n == 0 || steps[0].type != TSQueryPredicateStepTypeString) {
    return 1;
  }
  op = ts_query_string_value_for_id(q, steps[0].value_id, &op_len);

  if (strncmp(op, "not-", 4) == 0) {
    negate = 1;
    op += 4;
  }

  if (strcmp(op, "eq?") == 0 || strcmp(op, "any-of?") == 0) {
    uint32_t i;
    int hit = 0;
    if (n < 3 || steps[1].type != TSQueryPredicateStepTypeCapture) {
      return 1;
    }
    if (!capture_text(m, steps[1].value_id, text, text_len, &cap, &cap_len)) {
      return 1;
    }
    for (i = 2; i < n; i++) {
      if (steps[i].type == TSQueryPredicateStepTypeString) {
        uint32_t arg_len;
        const char *arg =
            ts_query_string_value_for_id(q, steps[i].value_id, &arg_len);
        if (cap_len == arg_len && memcmp(cap, arg, cap_len) == 0) {
          hit = 1;
          break;
        }
      } else {
        /* capture-vs-capture comparison: not implemented, accept */
        return 1;
      }
    }
    return negate ? !hit : hit;
  }

  if (strcmp(op, "match?") == 0) {
    regex_t re;
    char *pat;
    char *sub;
    uint32_t pat_len;
    const char *pat_src;
    int hit;

    if (n < 3 || steps[1].type != TSQueryPredicateStepTypeCapture ||
        steps[2].type != TSQueryPredicateStepTypeString) {
      return 1;
    }
    if (!capture_text(m, steps[1].value_id, text, text_len, &cap, &cap_len)) {
      return 1;
    }
    pat_src = ts_query_string_value_for_id(q, steps[2].value_id, &pat_len);

    pat = malloc(pat_len + 1);
    sub = malloc(cap_len + 1);
    if (!pat || !sub) {
      free(pat);
      free(sub);
      return 1;
    }
    memcpy(pat, pat_src, pat_len);
    pat[pat_len] = '\0';
    memcpy(sub, cap, cap_len);
    sub[cap_len] = '\0';

    if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) {
      free(pat);
      free(sub);
      return 1;
    }
    hit = regexec(&re, sub, 0, NULL, 0) == 0;
    regfree(&re);
    free(pat);
    free(sub);
    return negate ? !hit : hit;
  }

  /* #set! and anything else unknown: accept. */
  return 1;
}

static int match_passes(const TSQuery *q, const TSQueryMatch *m,
                        const char *text, uint32_t text_len) {
  uint32_t n_steps;
  const TSQueryPredicateStep *steps =
      ts_query_predicates_for_pattern(q, m->pattern_index, &n_steps);
  uint32_t i = 0;

  while (i < n_steps) {
    uint32_t j = i;
    while (j < n_steps && steps[j].type != TSQueryPredicateStepTypeDone) {
      j++;
    }
    if (!eval_predicate(q, m, text, text_len, steps + i, j - i)) {
      return 0;
    }
    i = j + 1;
  }
  return 1;
}

/* ---- span collection ---- */

typedef struct {
  uint32_t start;
  uint32_t end;
  const char *capture; /* borrowed from the query */
  uint32_t capture_len;
  uint32_t pattern; /* pattern index in the query -- the override order */
  uint32_t order;
} weft_ts_span;

static int span_cmp(const void *pa, const void *pb) {
  const weft_ts_span *a = pa;
  const weft_ts_span *b = pb;
  if (a->start != b->start) {
    return a->start < b->start ? -1 : 1;
  }
  if (a->end != b->end) {
    return a->end < b->end ? -1 : 1;
  }
  if (a->pattern != b->pattern) {
    return a->pattern < b->pattern ? -1 : 1;
  }
  if (a->order != b->order) {
    return a->order < b->order ? -1 : 1;
  }
  return 0;
}

/* ---- procs ---- */

static lcl_return_code c_ts_langs(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  lcl_value *list = lcl_list_new();
  const weft_ts_lang *l;

  (void)interp;
  (void)argc;
  (void)argv;
  for (l = WEFT_TS_LANGS; l->name; l++) {
    lcl_value *s = lcl_string_new(l->name);
    lcl_list_push(&list, s);
    lcl_ref_dec(s);
  }
  *out = list;
  return LCL_RC_OK;
}

static lcl_return_code c_ts_open(lcl_interp *interp, int argc, lcl_value **argv,
                                 lcl_value **out) {
  const char *name;
  const weft_ts_lang *l;
  char *query_src;
  weft_ts_session *s;
  uint32_t err_offset;
  TSQueryError err_type;

  if (argc < 1 || !(name = lcl_value_to_string(argv[0]))) {
    lcl_set_error(interp, "Ts::open: expected a language name");
    return LCL_RC_ERR;
  }
  l = lang_by_name(name);
  if (!l) {
    lcl_set_error(interp, "Ts::open: unknown language");
    return LCL_RC_ERR;
  }

  query_src = read_whole_file(l->query_path);
  if (!query_src) {
    lcl_set_error(interp, "Ts::open: cannot read highlight query");
    return LCL_RC_ERR;
  }

  s = calloc(1, sizeof(*s));
  if (!s) {
    free(query_src);
    lcl_set_error(interp, "Ts::open: out of memory");
    return LCL_RC_ERR;
  }
  snprintf(s->lang, sizeof(s->lang), "%s", l->name);

  s->parser = ts_parser_new();
  if (!s->parser || !ts_parser_set_language(s->parser, l->fn())) {
    session_finalize(s);
    free(query_src);
    lcl_set_error(interp, "Ts::open: failed to initialize parser");
    return LCL_RC_ERR;
  }

  s->query = ts_query_new(l->fn(), query_src, (uint32_t)strlen(query_src),
                          &err_offset, &err_type);
  free(query_src);
  if (!s->query) {
    session_finalize(s);
    lcl_set_error(interp, "Ts::open: highlight query failed to compile");
    return LCL_RC_ERR;
  }

  *out = lcl_opaque_new(s, WEFT_TS_SESSION_TAG, session_finalize);
  return LCL_RC_OK;
}

static lcl_return_code c_ts_lang(lcl_interp *interp, int argc, lcl_value **argv,
                                 lcl_value **out) {
  weft_ts_session *s;

  if (argc < 1 ||
      lcl_opaque_get(argv[0], WEFT_TS_SESSION_TAG, (void **)&s) != LCL_OK) {
    lcl_set_error(interp, "Ts::lang: expected a ts session");
    return LCL_RC_ERR;
  }
  *out = lcl_string_new(s->lang);
  return LCL_RC_OK;
}

static lcl_return_code c_ts_spans(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  weft_ts_session *s;
  const char *text;
  uint32_t text_len;
  TSTree *old;
  TSQueryCursor *cursor;
  TSQueryMatch m;
  weft_ts_span *spans = NULL;
  size_t count = 0, cap = 0, i;
  lcl_value *list;

  if (argc < 2 ||
      lcl_opaque_get(argv[0], WEFT_TS_SESSION_TAG, (void **)&s) != LCL_OK) {
    lcl_set_error(interp, "Ts::spans: expected a ts session and text");
    return LCL_RC_ERR;
  }
  if (s->closed) {
    lcl_set_error(interp, "Ts::spans: session is closed");
    return LCL_RC_ERR;
  }
  text = lcl_value_to_string(argv[1]);
  if (!text) {
    lcl_set_error(interp, "Ts::spans: expected text as a string");
    return LCL_RC_ERR;
  }
  text_len = (uint32_t)strlen(text);

  /* Incremental when a tree exists: Ts::edit has described every
   * change since the last parse (the documented contract above) --
   * reusing an UNedited tree was the legacy host's silent-misalign
   * bug, which is why the contract is load-bearing. */
  old = s->tree;
  s->tree = ts_parser_parse_string(s->parser, old, text, text_len);
  if (old) {
    ts_tree_delete(old);
  }
  if (!s->tree) {
    lcl_set_error(interp, "Ts::spans: parse failed");
    return LCL_RC_ERR;
  }

  cursor = ts_query_cursor_new();
  ts_query_cursor_exec(cursor, s->query, ts_tree_root_node(s->tree));

  while (ts_query_cursor_next_match(cursor, &m)) {
    uint16_t ci;
    if (!match_passes(s->query, &m, text, text_len)) {
      continue;
    }
    for (ci = 0; ci < m.capture_count; ci++) {
      uint32_t name_len;
      const char *name = ts_query_capture_name_for_id(
          s->query, m.captures[ci].index, &name_len);
      uint32_t start = ts_node_start_byte(m.captures[ci].node);
      uint32_t end = ts_node_end_byte(m.captures[ci].node);

      if (end <= start || name_len == 0 || name[0] == '_') {
        continue;
      }
      if (count == cap) {
        size_t next = cap ? cap * 2 : 256;
        weft_ts_span *grown = realloc(spans, next * sizeof(*spans));
        if (!grown) {
          free(spans);
          ts_query_cursor_delete(cursor);
          lcl_set_error(interp, "Ts::spans: out of memory");
          return LCL_RC_ERR;
        }
        spans = grown;
        cap = next;
      }
      spans[count].start = start;
      spans[count].end = end;
      spans[count].capture = name;
      spans[count].capture_len = name_len;
      spans[count].pattern = (uint32_t)m.pattern_index;
      spans[count].order = (uint32_t)count;
      count++;
    }
  }
  ts_query_cursor_delete(cursor);

  /* Same range captured by several patterns: the LAST pattern in the
   * QUERY wins (highlight queries are written bottom-override, e.g.
   * lcl's "keywords MUST BE LAST").  Emission order is NOT a proxy for
   * that: the cursor orders matches by match start, so a later pattern
   * whose match begins earlier (proc-params patterns anchored at the
   * `proc` keyword) would emit first and lose.  Sort by (range,
   * pattern index); keep each range group's last. */
  if (count > 1) {
    qsort(spans, count, sizeof(*spans), span_cmp);
  }

  list = lcl_list_new();
  for (i = 0; i < count; i++) {
    lcl_value *d;
    lcl_value *v;
    char namebuf[128];

    if (i + 1 < count && spans[i].start == spans[i + 1].start &&
        spans[i].end == spans[i + 1].end) {
      continue;
    }

    d = lcl_dict_new();
    v = lcl_int_new((long)spans[i].start);
    lcl_dict_put(&d, "start", v);
    lcl_ref_dec(v);
    v = lcl_int_new((long)spans[i].end);
    lcl_dict_put(&d, "end", v);
    lcl_ref_dec(v);
    snprintf(namebuf, sizeof(namebuf), "%.*s", (int)spans[i].capture_len,
             spans[i].capture);
    v = lcl_string_new(namebuf);
    lcl_dict_put(&d, "capture", v);
    lcl_ref_dec(v);

    lcl_list_push(&list, d);
    lcl_ref_dec(d);
  }
  free(spans);
  *out = list;
  return LCL_RC_OK;
}

/* Ts::edit session edits -- apply document deltas to the session's
 * tree so the next spans call can reparse incrementally.  Each edit
 * is #{start N deleted_len N inserted_len N} in SEQUENTIAL
 * coordinates (lk's delta contract: expressed in the state produced
 * by the preceding delta), which is exactly ts_tree_edit's
 * semantics -- apply in order.  Points are byte-degenerate (row 0,
 * column = byte): span consumers read bytes only, and weft's
 * equality gates pin incremental output == fresh parse.  Without a
 * tree this is a no-op (the next spans call full-parses anyway).
 *
 * CONTRACT: callers must report EVERY document change between spans
 * calls; a skipped edit silently misaligns captures (the legacy
 * host's bug).  weft::ts guarantees it with a doc subscription that
 * lives as long as the session. */
static lcl_return_code c_ts_edit(lcl_interp *interp, int argc, lcl_value **argv,
                                 lcl_value **out) {
  weft_ts_session *s;
  size_t i, n;

  if (argc < 2 ||
      lcl_opaque_get(argv[0], WEFT_TS_SESSION_TAG, (void **)&s) != LCL_OK) {
    lcl_set_error(interp, "Ts::edit: expected a ts session and edit list");
    return LCL_RC_ERR;
  }
  if (s->closed) {
    lcl_set_error(interp, "Ts::edit: session is closed");
    return LCL_RC_ERR;
  }

  n = lcl_list_len(argv[1]);
  for (i = 0; s->tree && i < n; i++) {
    lcl_value *d = NULL;
    lcl_value *v = NULL;
    long start = 0, del_len = 0, ins_len = 0;
    TSInputEdit e;

    if (lcl_list_get(argv[1], i, &d) != LCL_OK) {
      continue;
    }
    if (lcl_dict_get(d, "start", &v) == LCL_OK && v) {
      lcl_value_to_int(v, &start);
    }
    if (lcl_dict_get(d, "deleted_len", &v) == LCL_OK && v) {
      lcl_value_to_int(v, &del_len);
    }
    if (lcl_dict_get(d, "inserted_len", &v) == LCL_OK && v) {
      lcl_value_to_int(v, &ins_len);
    }
    lcl_ref_dec(d);

    e.start_byte = (uint32_t)start;
    e.old_end_byte = (uint32_t)(start + del_len);
    e.new_end_byte = (uint32_t)(start + ins_len);
    e.start_point.row = 0;
    e.start_point.column = e.start_byte;
    e.old_end_point.row = 0;
    e.old_end_point.column = e.old_end_byte;
    e.new_end_point.row = 0;
    e.new_end_point.column = e.new_end_byte;
    ts_tree_edit(s->tree, &e);
  }

  *out = lcl_int_new((long)n);
  return LCL_RC_OK;
}

static lcl_return_code c_ts_close(lcl_interp *interp, int argc,
                                  lcl_value **argv, lcl_value **out) {
  weft_ts_session *s;

  if (argc < 1 ||
      lcl_opaque_get(argv[0], WEFT_TS_SESSION_TAG, (void **)&s) != LCL_OK) {
    lcl_set_error(interp, "Ts::close: expected a ts session");
    return LCL_RC_ERR;
  }
  session_release(s);
  *out = lcl_int_new(1);
  return LCL_RC_OK;
}

void lcl_register_weft_ts(lcl_interp *interp) {
  lcl_value *ns = lcl_ns_new("Ts");
  lcl_define_take(interp, "Ts", ns);

  lcl_ns_def_take(ns, "langs", lcl_c_proc_new("Ts::langs", c_ts_langs));
  lcl_ns_def_take(ns, "open", lcl_c_proc_new("Ts::open", c_ts_open));
  lcl_ns_def_take(ns, "lang", lcl_c_proc_new("Ts::lang", c_ts_lang));
  lcl_ns_def_take(ns, "edit", lcl_c_proc_new("Ts::edit", c_ts_edit));
  lcl_ns_def_take(ns, "spans", lcl_c_proc_new("Ts::spans", c_ts_spans));
  lcl_ns_def_take(ns, "close", lcl_c_proc_new("Ts::close", c_ts_close));
}
