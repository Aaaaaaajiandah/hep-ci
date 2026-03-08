#ifndef HEP_CI_PIPELINE_H
#define HEP_CI_PIPELINE_H

#include <time.h>

#define CI_MAX_JOBS    32
#define CI_MAX_STEPS   64
#define CI_MAX_NEEDS   16
#define CI_MAX_STR     1024
#define CI_MAX_ENV     32

typedef enum {
    CI_STATUS_PENDING,
    CI_STATUS_RUNNING,
    CI_STATUS_PASS,
    CI_STATUS_FAIL,
    CI_STATUS_CANCELLED,
    CI_STATUS_SKIPPED,
} ci_status;

typedef struct {
    char name[CI_MAX_STR];
    char run[CI_MAX_STR];   /* shell command */
} ci_step;

typedef struct {
    char     name[CI_MAX_STR];
    ci_step  steps[CI_MAX_STEPS];
    int      nsteps;
    char     needs[CI_MAX_NEEDS][CI_MAX_STR]; /* job dependencies */
    int      nneeds;
    char     env_keys[CI_MAX_ENV][128];
    char     env_vals[CI_MAX_ENV][CI_MAX_STR];
    int      nenv;
    char     workdir[CI_MAX_STR];  /* optional working directory */
    int      timeout;              /* seconds, 0 = no limit */
} ci_job;

typedef struct {
    char     name[CI_MAX_STR];
    char     on_event[CI_MAX_STR]; /* push, manual, schedule */
    ci_job   jobs[CI_MAX_JOBS];
    int      njobs;
} ci_pipeline;

typedef struct {
    char       run_id[64];      /* timestamp-based unique id */
    char       pipeline[CI_MAX_STR];
    char       commit[41];
    char       branch[256];
    char       triggered_by[128];
    time_t     started;
    time_t     finished;
    ci_status  status;
    char       log_path[CI_MAX_STR];
} ci_run;

/* parse .hep-ci.yml into pipeline */
int  pipeline_parse(const char *yml_path, ci_pipeline *p, char *err, int err_max);
void pipeline_free(ci_pipeline *p);

/* execute a pipeline, write logs to log_path, return 0=pass */
int  pipeline_run(ci_pipeline *p, const char *repo_root,
                  const char *log_path, const char *run_id);

/* run storage */
int  run_save(const char *ci_dir, ci_run *run);
int  run_load(const char *ci_dir, const char *run_id, ci_run *run);
int  run_list(const char *ci_dir, ci_run **out, int *count);
void run_list_free(ci_run *runs, int count);

static inline const char *ci_status_str(ci_status s) {
    switch (s) {
        case CI_STATUS_PENDING:   return "pending";
        case CI_STATUS_RUNNING:   return "running";
        case CI_STATUS_PASS:      return "pass";
        case CI_STATUS_FAIL:      return "fail";
        case CI_STATUS_CANCELLED: return "cancelled";
        case CI_STATUS_SKIPPED:   return "skipped";
        default:                  return "unknown";
    }
}

static inline const char *ci_status_color(ci_status s) {
    switch (s) {
        case CI_STATUS_PASS:      return "\033[32m";
        case CI_STATUS_FAIL:      return "\033[31m";
        case CI_STATUS_RUNNING:   return "\033[33m";
        case CI_STATUS_CANCELLED: return "\033[35m";
        default:                  return "\033[0m";
    }
}

#endif
