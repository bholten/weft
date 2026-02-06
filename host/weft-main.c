/* weft-main.c -- the weft host binary.
 *
 * Deliberately small (plan section 3): create the interpreter, register
 * lcl core + the package matrix + lk, publish host facts as globals,
 * load the lk DSL prelude, and hand off to app/boot.lcl.  Everything
 * after that -- capability registry, workspace, UI -- is Lcl.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lcl.h>

#ifdef WEFT_HAVE_IO
#include <lcl-io.h>
#endif
#ifdef WEFT_HAVE_POSIX
#include <lcl-posix.h>
#endif
#ifdef WEFT_HAVE_REGEX
#include <lcl-regex.h>
#endif
#ifdef WEFT_HAVE_PROCESS
#include <signal.h>
#include <lcl-process.h>
#endif
#ifdef WEFT_HAVE_JSON
#include <lcl-json.h>
#endif
#ifdef WEFT_HAVE_TIME
#include <lcl-time.h>
#endif

#include "lcl-lk.h"

#ifdef WEFT_HAVE_TS
void lcl_register_weft_ts(lcl_interp *interp);
#endif

#define WEFT_VERSION_STRING "0.1.0-dev"

static const char *env_or(const char *env_name, const char *fallback) {
  const char *v = getenv(env_name);
  return (v && v[0]) ? v : fallback;
}

static void define_str(lcl_interp *interp, const char *name,
                       const char *value) {
  lcl_define_take(interp, name, lcl_string_new(value));
}

static void define_int(lcl_interp *interp, const char *name, long value) {
  lcl_define_take(interp, name, lcl_int_new(value));
}

static int eval_file_reporting(lcl_interp *interp, const char *path,
                               const char *what) {
  lcl_value *result = NULL;
  int rc = lcl_eval_file(interp, path, &result);

  if (rc != LCL_RC_OK) {
    const char *file = lcl_interp_error_file(interp);
    int line = lcl_interp_error_line(interp);
    const char *msg = lcl_interp_error_msg(interp);

    fprintf(stderr, "weft: error loading %s (%s)", what, path);
    if (file) {
      fprintf(stderr, " in %s", file);
    }
    if (line > 0) {
      fprintf(stderr, ":%d", line);
    }
    if (msg) {
      fprintf(stderr, ": %s", msg);
    }
    fprintf(stderr, "\n");
  }

  if (result) {
    lcl_ref_dec(result);
  }
  return rc;
}

int main(int argc, char **argv) {
  lcl_interp *interp;
  lcl_value *files;
  const char *script = NULL;
  int headless = 0;
  int rc;
  int i;
  char user_config[1024];
  const char *home;

#ifdef WEFT_HAVE_PROCESS
  /* A filter child that exits before reading its stdin (|head -1 on a
   * large selection, |false) would otherwise take the whole image
   * down with SIGPIPE: the write fails with EPIPE instead, and the
   * filter module stops feeding.  Hosts own effects. */
  signal(SIGPIPE, SIG_IGN);
#endif

  interp = lcl_interp_new();
  if (!interp) {
    fprintf(stderr, "weft: failed to create interpreter\n");
    return 1;
  }

  lcl_register_core(interp);
  lcl_register_lk(interp);
#ifdef WEFT_HAVE_IO
  lcl_register_io(interp);
#endif
#ifdef WEFT_HAVE_POSIX
  lcl_register_posix(interp);
#endif
#ifdef WEFT_HAVE_REGEX
  lcl_register_regex(interp);
#endif
#ifdef WEFT_HAVE_PROCESS
  lcl_register_process(interp);
#endif
#ifdef WEFT_HAVE_JSON
  lcl_register_json(interp);
#endif
#ifdef WEFT_HAVE_TIME
  lcl_register_time(interp);
#endif
#ifdef WEFT_HAVE_TS
  lcl_register_weft_ts(interp);
#endif

  /* ---- Arguments: weft [--headless script.lcl] [files...] ---- */
  files = lcl_list_new();
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--version") == 0) {
      printf("weft %s\n", WEFT_VERSION_STRING);
      lcl_ref_dec(files);
      lcl_interp_free(interp);
      return 0;
    } else if (strcmp(argv[i], "--headless") == 0) {
      headless = 1;
      if (i + 1 < argc) {
        script = argv[++i];
      }
    } else {
      lcl_list_push(&files, lcl_string_new(argv[i]));
    }
  }

  /* ---- Host facts, read by app/boot.lcl ---- */
  home = getenv("HOME");
  snprintf(user_config, sizeof(user_config), "%s/.config/weft/init.lcl",
           home ? home : "");

  define_str(interp, "weft_version", WEFT_VERSION_STRING);
  define_str(interp, "weft_host_kind", "posix");
  define_str(interp, "weft_root", env_or("WEFT_ROOT", WEFT_ROOT_DEFAULT));
  define_str(interp, "weft_lib_dir",
             env_or("WEFT_LIB_DIR", WEFT_LIB_DIR_DEFAULT));
  /* Bare `require weft/...` resolves here regardless of launch CWD. */
  lcl_add_require_root(interp, env_or("WEFT_LIB_DIR", WEFT_LIB_DIR_DEFAULT));
  define_str(interp, "weft_lcl_lib_dir",
             env_or("WEFT_LCL_LIB_DIR", WEFT_LCL_LIB_DIR_DEFAULT));
  /* lcl's Lcl-implemented libraries: require "doc/src/Doc.lcl". */
  lcl_add_require_root(interp,
                       env_or("WEFT_LCL_LIB_DIR", WEFT_LCL_LIB_DIR_DEFAULT));
  define_str(interp, "weft_config_dir",
             env_or("WEFT_CONFIG_DIR", WEFT_CONFIG_DIR_DEFAULT));
  define_str(interp, "weft_user_config", user_config);
  define_str(interp, "weft_script", script ? script : "");
  define_int(interp, "weft_headless", headless);
  lcl_define(interp, "weft_files", files);
  lcl_ref_dec(files);

#ifdef WEFT_HAVE_IO
  define_int(interp, "weft_have_io", 1);
#else
  define_int(interp, "weft_have_io", 0);
#endif
#ifdef WEFT_HAVE_POSIX
  define_int(interp, "weft_have_posix", 1);
#else
  define_int(interp, "weft_have_posix", 0);
#endif
#ifdef WEFT_HAVE_REGEX
  define_int(interp, "weft_have_regex", 1);
#else
  define_int(interp, "weft_have_regex", 0);
#endif
#ifdef WEFT_HAVE_PROCESS
  define_int(interp, "weft_have_process", 1);
#else
  define_int(interp, "weft_have_process", 0);
#endif
#ifdef WEFT_HAVE_JSON
  define_int(interp, "weft_have_json", 1);
#else
  define_int(interp, "weft_have_json", 0);
#endif
#ifdef WEFT_HAVE_TIME
  define_int(interp, "weft_have_time", 1);
#else
  define_int(interp, "weft_have_time", 0);
#endif
#ifdef WEFT_HAVE_TS
  define_int(interp, "weft_have_ts", 1);
#else
  define_int(interp, "weft_have_ts", 0);
#endif

  /* ---- lk DSL prelude, then boot ---- */
#ifdef LK_DSL_PATH
  if (eval_file_reporting(interp, LK_DSL_PATH, "lk DSL prelude") !=
      LCL_RC_OK) {
    lcl_interp_free(interp);
    return 1;
  }
#endif

  rc = eval_file_reporting(interp, env_or("WEFT_BOOT", WEFT_BOOT_DEFAULT),
                           "boot");

  lcl_interp_free(interp);
  return rc == LCL_RC_OK ? 0 : 1;
}
