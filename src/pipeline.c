#include "pipeline.h"
#include "yaml.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>

/* ── pipeline_parse ──────────────────────────────────────────────────────── */

int pipeline_parse(const char *yml_path, ci_pipeline *p,
                   char *err, int err_max) {
    memset(p, 0, sizeof(*p));

    /* read file */
    FILE *f = fopen(yml_path, "r");
    if (!f) {
        snprintf(err, err_max, "cannot open '%s'", yml_path);
        return -1;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *text = malloc(sz + 1);
    fread(text, 1, sz, f); text[sz] = '\0'; fclose(f);

    yaml_doc doc;
    if (yaml_parse(text, &doc) != 0) {
        snprintf(err, err_max, "YAML parse error: %s", doc.error);
        free(text); return -1;
    }
    free(text);

    /* name */
    strncpy(p->name, yaml_str(&doc.root, "name", "pipeline"),
            CI_MAX_STR - 1);

    /* on: event */
    yaml_node *on = yaml_get(&doc.root, "on");
    if (on && on->type == YAML_SCALAR)
        strncpy(p->on_event, on->val, CI_MAX_STR - 1);
    else
        strncpy(p->on_event, "push", CI_MAX_STR - 1);

    /* jobs: */
    yaml_node *jobs = yaml_get(&doc.root, "jobs");
    if (!jobs) {
        snprintf(err, err_max, "no 'jobs' section found");
        yaml_free(&doc); return -1;
    }

    for (int ji = 0; ji < jobs->nchildren && p->njobs < CI_MAX_JOBS; ji++) {
        yaml_node *jnode = &jobs->children[ji];
        ci_job *job = &p->jobs[p->njobs++];
        memset(job, 0, sizeof(*job));
        strncpy(job->name, jnode->key[0] ? jnode->key : jnode->val,
                CI_MAX_STR - 1);

        /* timeout */
        const char *to = yaml_str(jnode, "timeout", "0");
        job->timeout = atoi(to);

        /* workdir */
        const char *wd = yaml_str(jnode, "workdir", "");
        strncpy(job->workdir, wd, CI_MAX_STR - 1);

        /* needs: */
        yaml_node *needs = yaml_get(jnode, "needs");
        if (needs) {
            if (needs->type == YAML_SCALAR) {
                strncpy(job->needs[job->nneeds++], needs->val,
                        CI_MAX_STR - 1);
            } else {
                for (int ni = 0; ni < needs->nchildren &&
                     job->nneeds < CI_MAX_NEEDS; ni++) {
                    strncpy(job->needs[job->nneeds++],
                            needs->children[ni].val, CI_MAX_STR - 1);
                }
            }
        }

        /* env: */
        yaml_node *env = yaml_get(jnode, "env");
        if (env) {
            for (int ei = 0; ei < env->nchildren &&
                 job->nenv < CI_MAX_ENV; ei++) {
                strncpy(job->env_keys[job->nenv],
                        env->children[ei].key, 127);
                strncpy(job->env_vals[job->nenv],
                        env->children[ei].val, CI_MAX_STR - 1);
                job->nenv++;
            }
        }

        /* steps: */
        yaml_node *steps = yaml_get(jnode, "steps");
        if (!steps) continue;

        for (int si = 0; si < steps->nchildren &&
             job->nsteps < CI_MAX_STEPS; si++) {
            yaml_node *snode = &steps->children[si];
            ci_step *step = &job->steps[job->nsteps++];
            memset(step, 0, sizeof(*step));

            /* step can be: {name: x, run: cmd} or just a scalar command */
            if (snode->type == YAML_SCALAR) {
                snprintf(step->name, CI_MAX_STR, "step %d", job->nsteps);
                strncpy(step->run, snode->val, CI_MAX_STR - 1);
            } else {
                const char *sname = yaml_str(snode, "name", "");
                const char *srun  = yaml_str(snode, "run",  "");
                strncpy(step->name, sname[0] ? sname : "step", CI_MAX_STR-1);
                strncpy(step->run,  srun,  CI_MAX_STR - 1);
            }
        }
    }

    yaml_free(&doc);
    return 0;
}

void pipeline_free(ci_pipeline *p) {
    (void)p; /* everything is stack-allocated in ci_pipeline */
}

/* ── pipeline_run ────────────────────────────────────────────────────────── */

/* run a single step, append output to log_fd, return exit code */
static int run_step(ci_step *step, ci_job *job,
                    const char *repo_root, int log_fd) {
    /* log step header */
    char hdr[CI_MAX_STR];
    int hlen = snprintf(hdr, sizeof(hdr),
                        "\n── step: %s ──\n$ %s\n", step->name, step->run);
    write(log_fd, hdr, hlen);

    pid_t pid = fork();
    if (pid == 0) {
        /* child: redirect stdout/stderr to log */
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);

        /* set working directory */
        if (job->workdir[0]) chdir(job->workdir);
        else chdir(repo_root);

        /* set env vars */
        for (int i = 0; i < job->nenv; i++)
            setenv(job->env_keys[i], job->env_vals[i], 1);

        /* set HEP_CI env */
        setenv("HEP_CI", "1", 1);
        setenv("HEP_REPO", repo_root, 1);

        execl("/bin/sh", "sh", "-c", step->run, NULL);
        exit(127);
    }
    if (pid < 0) return -1;

    /* wait with optional timeout */
    int status;
    if (job->timeout > 0) {
        /* poll with alarm */
        alarm((unsigned)job->timeout);
        waitpid(pid, &status, 0);
        alarm(0);
    } else {
        waitpid(pid, &status, 0);
    }

    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    char result[64];
    int rlen = snprintf(result, sizeof(result),
                        "── exit: %d ──\n", code);
    write(log_fd, result, rlen);
    return code;
}

int pipeline_run(ci_pipeline *p, const char *repo_root,
                 const char *log_path, const char *run_id) {
    /* open log file */
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) return -1;

    /* write run header */
    time_t now = time(NULL);
    char tstr[32]; strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S",
                             localtime(&now));
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "hep-ci run %s\npipeline: %s\nstarted:  %s\nrepo:     %s\n"
        "═══════════════════════════════════════\n",
        run_id, p->name, tstr, repo_root);
    write(log_fd, hdr, hlen);

    /* track which jobs have passed for dependency resolution */
    int job_done[CI_MAX_JOBS]   = {0};
    int job_passed[CI_MAX_JOBS] = {0};
    int overall = 0;

    /* simple topological execution — up to njobs passes */
    int executed = 0;
    for (int pass = 0; pass < p->njobs && executed < p->njobs; pass++) {
        for (int ji = 0; ji < p->njobs; ji++) {
            if (job_done[ji]) continue;
            ci_job *job = &p->jobs[ji];

            /* check all dependencies satisfied */
            int deps_ok = 1;
            for (int ni = 0; ni < job->nneeds; ni++) {
                int found = 0, dep_passed = 0;
                for (int ki = 0; ki < p->njobs; ki++) {
                    if (strcmp(p->jobs[ki].name, job->needs[ni]) == 0) {
                        found = 1;
                        if (job_done[ki] && job_passed[ki]) dep_passed = 1;
                        if (job_done[ki] && !job_passed[ki]) {
                            /* dep failed — skip this job */
                            char skip[256];
                            int slen = snprintf(skip, sizeof(skip),
                                "\n══ job: %s — SKIPPED (dep %s failed)\n",
                                job->name, job->needs[ni]);
                            write(log_fd, skip, slen);
                            job_done[ji] = 1; job_passed[ji] = 0;
                            executed++;
                        }
                        break;
                    }
                }
                if (!found || !dep_passed) { deps_ok = 0; break; }
                if (job_done[ji]) break;
            }
            if (job_done[ji]) continue;
            if (!deps_ok) continue;

            /* run this job */
            char jhdr[256];
            int jlen = snprintf(jhdr, sizeof(jhdr),
                "\n══ job: %s ══\n", job->name);
            write(log_fd, jhdr, jlen);

            int job_ok = 1;
            for (int si = 0; si < job->nsteps; si++) {
                int rc = run_step(&job->steps[si], job, repo_root, log_fd);
                if (rc != 0) {
                    job_ok = 0;
                    char fail[128];
                    int flen = snprintf(fail, sizeof(fail),
                        "step '%s' failed (exit %d) — stopping job\n",
                        job->steps[si].name, rc);
                    write(log_fd, fail, flen);
                    break;
                }
            }

            job_done[ji]   = 1;
            job_passed[ji] = job_ok;
            if (!job_ok) overall = 1;
            executed++;

            char jresult[128];
            int jrlen = snprintf(jresult, sizeof(jresult),
                "══ job: %s — %s ══\n",
                job->name, job_ok ? "PASS" : "FAIL");
            write(log_fd, jresult, jrlen);
        }
    }

    /* footer */
    time_t done = time(NULL);
    strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", localtime(&done));
    char footer[256];
    int flen = snprintf(footer, sizeof(footer),
        "\n═══════════════════════════════════════\n"
        "finished: %s\nduration: %lds\nresult:   %s\n",
        tstr, (long)(done - now), overall ? "FAIL" : "PASS");
    write(log_fd, footer, flen);

    close(log_fd);
    return overall;
}

/* ── run storage ─────────────────────────────────────────────────────────── */

static void run_to_line(ci_run *r, char *line, int max) {
    snprintf(line, max, "%s|%s|%s|%s|%s|%ld|%ld|%d|%s\n",
             r->run_id, r->pipeline, r->commit, r->branch,
             r->triggered_by,
             (long)r->started, (long)r->finished,
             (int)r->status, r->log_path);
}

static int line_to_run(const char *line, ci_run *r) {
    memset(r, 0, sizeof(*r));
    long started, finished; int status;
    int n = sscanf(line, "%63[^|]|%1023[^|]|%40[^|]|%255[^|]|%127[^|]|%ld|%ld|%d|%1023[^\n]",
                   r->run_id, r->pipeline, r->commit, r->branch,
                   r->triggered_by, &started, &finished, &status,
                   r->log_path);
    r->started  = (time_t)started;
    r->finished = (time_t)finished;
    r->status   = (ci_status)status;
    return n >= 8 ? 0 : -1;
}

int run_save(const char *ci_dir, ci_run *run) {
    mkdir(ci_dir, 0755);
    char path[CI_MAX_STR];
    snprintf(path, sizeof(path), "%s/runs.log", ci_dir);
    FILE *f = fopen(path, "a");
    if (!f) return -1;
    char line[CI_MAX_STR * 4];
    run_to_line(run, line, sizeof(line));
    fputs(line, f);
    fclose(f);
    return 0;
}

int run_load(const char *ci_dir, const char *run_id, ci_run *run) {
    char path[CI_MAX_STR];
    snprintf(path, sizeof(path), "%s/runs.log", ci_dir);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[CI_MAX_STR * 4];
    while (fgets(line, sizeof(line), f)) {
        ci_run tmp;
        if (line_to_run(line, &tmp) == 0 &&
            strcmp(tmp.run_id, run_id) == 0) {
            *run = tmp;
            fclose(f); return 0;
        }
    }
    fclose(f);
    return -1;
}

int run_list(const char *ci_dir, ci_run **out, int *count) {
    *out = NULL; *count = 0;
    char path[CI_MAX_STR];
    snprintf(path, sizeof(path), "%s/runs.log", ci_dir);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    *out = malloc(1024 * sizeof(ci_run));
    char line[CI_MAX_STR * 4];
    while (fgets(line, sizeof(line), f) && *count < 1024) {
        if (line_to_run(line, &(*out)[*count]) == 0)
            (*count)++;
    }
    fclose(f);
    return 0;
}

void run_list_free(ci_run *runs, int count) {
    (void)count;
    free(runs);
}
