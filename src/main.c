#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>

#include "pipeline.h"
#include "yaml.h"

#define CI_DIR        ".hep-ci"
#define CI_YML        ".hep-ci.yml"
#define CI_SERVE_PORT 7071
#define CI_MAX_PATH   2048

/* ── find repo root (walk up looking for .hep) ───────────────────────────── */
static int find_repo_root(char *out, int max) {
    char cwd[CI_MAX_PATH]; getcwd(cwd, sizeof(cwd));
    char try[CI_MAX_PATH];
    strncpy(try, cwd, CI_MAX_PATH - 1);
    for (int i = 0; i < 20; i++) {
        char hep[CI_MAX_PATH];
        snprintf(hep, sizeof(hep), "%s/.hep", try);
        struct stat st;
        if (stat(hep, &st) == 0 && S_ISDIR(st.st_mode)) {
            strncpy(out, try, max - 1);
            return 0;
        }
        char parent[CI_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s/..", try);
        realpath(parent, try);
        if (strcmp(try, "/") == 0) break;
    }
    strncpy(out, cwd, max - 1);
    return 0; /* fallback to cwd */
}

/* ── generate run_id ─────────────────────────────────────────────────────── */
static void make_run_id(char *out) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    snprintf(out, 64, "%04d%02d%02d-%02d%02d%02d",
             tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

/* ── read HEAD commit from .hep ──────────────────────────────────────────── */
static void read_head(const char *repo_root, char *hex_out, char *branch_out) {
    hex_out[0] = '\0'; branch_out[0] = '\0';
    char head_path[CI_MAX_PATH];
    snprintf(head_path, sizeof(head_path), "%s/.hep/HEAD", repo_root);
    FILE *f = fopen(head_path, "r");
    if (!f) return;
    char line[256]; fgets(line, sizeof(line), f); fclose(f);
    line[strcspn(line, "\n")] = '\0';
    if (strncmp(line, "ref: ", 5) == 0) {
        char ref[256]; sscanf(line+5, "%255s", ref);
        /* extract branch name */
        const char *bn = strrchr(ref, '/');
        strncpy(branch_out, bn ? bn+1 : ref, 255);
        /* resolve ref */
        char rpath[CI_MAX_PATH];
        snprintf(rpath, sizeof(rpath), "%s/.hep/%s", repo_root, ref);
        FILE *rf = fopen(rpath, "r");
        if (rf) { fscanf(rf, "%40s", hex_out); fclose(rf); }
    } else {
        strncpy(hex_out, line, 40);
        strncpy(branch_out, "HEAD", 255);
    }
}

/* ═════════════════════════════════════════════════════════════════════════
 * COMMANDS
 * ═════════════════════════════════════════════════════════════════════════ */

/* ── hep-ci init ─────────────────────────────────────────────────────────── */
static int cmd_init(int argc, char **argv) {
    (void)argc; (void)argv;

    struct stat st;
    if (stat(CI_YML, &st) == 0) {
        printf("init: %s already exists\n", CI_YML);
        return 1;
    }

    FILE *f = fopen(CI_YML, "w");
    if (!f) { perror("init"); return 1; }

    fprintf(f,
        "name: my pipeline\n"
        "\n"
        "on: push\n"
        "\n"
        "jobs:\n"
        "  build:\n"
        "    steps:\n"
        "      - name: compile\n"
        "        run: echo 'add your build command here'\n"
        "      - name: test\n"
        "        run: echo 'add your test command here'\n"
        "\n"
        "  # example: job that only runs after build passes\n"
        "  # deploy:\n"
        "  #   needs: build\n"
        "  #   steps:\n"
        "  #     - name: ship\n"
        "  #       run: ./deploy.sh\n"
    );
    fclose(f);

    printf("init: created %s\n", CI_YML);
    printf("      edit it, then run 'hep-ci run' to test locally\n");
    printf("      hep-ci serve  starts the daemon for push-triggered runs\n");
    return 0;
}

/* ── hep-ci run ──────────────────────────────────────────────────────────── */
static int cmd_run(int argc, char **argv) {
    const char *yml = CI_YML;
    if (argc >= 2) yml = argv[1];

    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));

    /* parse pipeline */
    ci_pipeline p;
    char err[256];
    if (pipeline_parse(yml, &p, err, sizeof(err)) != 0) {
        fprintf(stderr, "run: %s\n", err); return 1;
    }

    printf("hep-ci run\n");
    printf("pipeline : %s\n", p.name);
    printf("jobs     : %d\n", p.njobs);
    printf("repo     : %s\n\n", repo_root);

    /* set up ci dir and log path */
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);
    mkdir(ci_dir, 0755);
    char logs_dir[CI_MAX_PATH];
    snprintf(logs_dir, sizeof(logs_dir), "%s/logs", ci_dir);
    mkdir(logs_dir, 0755);

    char run_id[64]; make_run_id(run_id);
    char log_path[CI_MAX_PATH];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", logs_dir, run_id);

    /* read commit info */
    char commit[41], branch[256];
    read_head(repo_root, commit, branch);

    /* record run start */
    ci_run run = {0};
    strncpy(run.run_id, run_id, 63);
    strncpy(run.pipeline, p.name, CI_MAX_STR-1);
    strncpy(run.commit, commit[0] ? commit : "none", 40);
    strncpy(run.branch, branch[0] ? branch : "unknown", 255);
    strncpy(run.triggered_by, "manual", 127);
    run.started = time(NULL);
    run.status  = CI_STATUS_RUNNING;
    strncpy(run.log_path, log_path, CI_MAX_STR-1);
    run_save(ci_dir, &run);

    printf("run id   : %s\n", run_id);
    printf("log      : %s\n\n", log_path);

    /* execute */
    int rc = pipeline_run(&p, repo_root, log_path, run_id);

    /* update run record */
    run.finished = time(NULL);
    run.status   = rc == 0 ? CI_STATUS_PASS : CI_STATUS_FAIL;

    /* rewrite the last line of runs.log with updated status */
    char runs_path[CI_MAX_PATH];
    snprintf(runs_path, sizeof(runs_path), "%s/runs.log", ci_dir);
    /* append corrected entry (status() will show the latest for a given id) */
    run_save(ci_dir, &run);

    /* print result */
    const char *col = ci_status_color(run.status);
    printf("\n%s%s\033[0m — %lds\n",
           col,
           run.status == CI_STATUS_PASS ? "PASS ✓" : "FAIL ✗",
           (long)(run.finished - run.started));

    if (rc != 0) {
        printf("run 'hep-ci logs %s' to see output\n", run_id);
    }

    pipeline_free(&p);
    return rc;
}

/* ── hep-ci status ───────────────────────────────────────────────────────── */
static int cmd_status(int argc, char **argv) {
    int limit = 10;
    if (argc >= 2) limit = atoi(argv[1]);

    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);

    ci_run *runs; int count;
    run_list(ci_dir, &runs, &count);

    if (count == 0) {
        printf("status: no runs yet — use 'hep-ci run' to start one\n");
        return 0;
    }

    printf("%-20s  %-8s  %-12s  %-10s  %-8s  %s\n",
           "run id", "status", "pipeline", "branch", "duration", "commit");
    printf("%-20s  %-8s  %-12s  %-10s  %-8s  %s\n",
           "────────────────────", "────────",
           "────────────", "──────────", "────────", "───────");

    /* show most recent 'limit' runs — iterate from end */
    int start = count - limit;
    if (start < 0) start = 0;

    /* deduplicate: for same run_id show last entry (most recent status) */
    /* simple: walk backwards, skip already-shown ids */
    char shown[1024][64]; int nshown = 0;
    for (int i = count - 1; i >= 0 && nshown < limit; i--) {
        ci_run *r = &runs[i];
        int dup = 0;
        for (int j = 0; j < nshown; j++)
            if (strcmp(shown[j], r->run_id) == 0) { dup = 1; break; }
        if (dup) continue;
        strncpy(shown[nshown++], r->run_id, 63);

        long dur = r->finished > r->started ?
                   (long)(r->finished - r->started) : 0;
        char dur_str[16];
        if (dur < 60) snprintf(dur_str, sizeof(dur_str), "%lds", dur);
        else          snprintf(dur_str, sizeof(dur_str), "%ldm%lds",
                               dur/60, dur%60);

        char sh[8] = "none   ";
        if (r->commit[0]) { memcpy(sh, r->commit, 7); sh[7]='\0'; }

        const char *col = ci_status_color(r->status);
        printf("%-20s  %s%-8s\033[0m  %-12s  %-10s  %-8s  %s\n",
               r->run_id,
               col, ci_status_str(r->status),
               r->pipeline, r->branch, dur_str, sh);
    }

    run_list_free(runs, count);
    return 0;
}

/* ── hep-ci logs ─────────────────────────────────────────────────────────── */
static int cmd_logs(int argc, char **argv) {
    if (argc < 2) {
        /* show logs for most recent run */
        char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
        char ci_dir[CI_MAX_PATH];
        snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);
        ci_run *runs; int count;
        run_list(ci_dir, &runs, &count);
        if (count == 0) {
            printf("logs: no runs yet\n"); return 0;
        }
        /* find most recent unique run */
        char last_id[64] = {0};
        for (int i = count-1; i >= 0; i--) {
            if (!last_id[0] || strcmp(runs[i].run_id, last_id) != 0) {
                strncpy(last_id, runs[i].run_id, 63);
                break;
            }
        }
        run_list_free(runs, count);
        char *new_argv[] = { argv[0], last_id };
        return cmd_logs(2, new_argv);
    }

    const char *run_id = argv[1];
    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);

    ci_run run;
    if (run_load(ci_dir, run_id, &run) != 0) {
        fprintf(stderr, "logs: run '%s' not found\n", run_id); return 1;
    }

    FILE *f = fopen(run.log_path, "r");
    if (!f) {
        fprintf(stderr, "logs: log file not found: %s\n", run.log_path);
        return 1;
    }
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        write(STDOUT_FILENO, buf, n);
    fclose(f);
    return 0;
}

/* ── hep-ci watch ────────────────────────────────────────────────────────── */
static int cmd_watch(int argc, char **argv) {
    (void)argc; (void)argv;

    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);

    /* find most recent running job */
    ci_run *runs; int count;
    run_list(ci_dir, &runs, &count);

    char log_path[CI_MAX_PATH] = {0};
    char run_id[64] = {0};
    for (int i = count-1; i >= 0; i--) {
        if (runs[i].status == CI_STATUS_RUNNING) {
            strncpy(log_path, runs[i].log_path, CI_MAX_PATH-1);
            strncpy(run_id, runs[i].run_id, 63);
            break;
        }
    }
    run_list_free(runs, count);

    if (!log_path[0]) {
        printf("watch: no running jobs\n"); return 0;
    }

    printf("watch: tailing run %s (Ctrl+C to stop)\n\n", run_id);

    /* tail the log file */
    FILE *f = fopen(log_path, "r");
    if (!f) { fprintf(stderr, "watch: cannot open log\n"); return 1; }

    while (1) {
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            write(STDOUT_FILENO, buf, n);

        /* check if run is still active */
        ci_run cur;
        if (run_load(ci_dir, run_id, &cur) == 0 &&
            cur.status != CI_STATUS_RUNNING) {
            /* drain remaining output */
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                write(STDOUT_FILENO, buf, n);
            const char *col = ci_status_color(cur.status);
            printf("\n%s%s\033[0m\n", col,
                   cur.status == CI_STATUS_PASS ? "PASS ✓" : "FAIL ✗");
            break;
        }
        usleep(250000); /* 250ms poll */
    }
    fclose(f);
    return 0;
}

/* ── hep-ci history ──────────────────────────────────────────────────────── */
static int cmd_history(int argc, char **argv) {
    (void)argc; (void)argv;

    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);

    ci_run *runs; int count;
    run_list(ci_dir, &runs, &count);

    if (count == 0) { printf("history: no runs\n"); return 0; }

    /* dedup by run_id, keep last status */
    int pass = 0, fail = 0, total = 0;
    long total_dur = 0;
    char seen[1024][64]; int nseen = 0;

    printf("full run history:\n\n");
    for (int i = count-1; i >= 0; i--) {
        ci_run *r = &runs[i];
        int dup = 0;
        for (int j = 0; j < nseen; j++)
            if (strcmp(seen[j], r->run_id) == 0) { dup = 1; break; }
        if (dup) continue;
        strncpy(seen[nseen++], r->run_id, 63);

        char tstr[32];
        strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M",
                 localtime(&r->started));
        long dur = r->finished > r->started ?
                   (long)(r->finished - r->started) : 0;
        char dur_str[16];
        if (dur < 60) snprintf(dur_str, sizeof(dur_str), "%lds", dur);
        else          snprintf(dur_str, sizeof(dur_str), "%ldm%lds", dur/60, dur%60);

        const char *col = ci_status_color(r->status);
        printf("  %s%-8s\033[0m  %s  %s  branch:%-12s  %s\n",
               col, ci_status_str(r->status),
               r->run_id, tstr, r->branch, r->pipeline);

        if (r->status == CI_STATUS_PASS) pass++;
        if (r->status == CI_STATUS_FAIL) fail++;
        if (r->status == CI_STATUS_PASS || r->status == CI_STATUS_FAIL) {
            total++; total_dur += dur;
        }
    }

    printf("\n%d total  |  %d pass  |  %d fail",
           total, pass, fail);
    if (total > 0) {
        long avg = total_dur / total;
        printf("  |  avg duration: ");
        if (avg < 60) printf("%lds", avg);
        else printf("%ldm%lds", avg/60, avg%60);
    }
    printf("\n");

    run_list_free(runs, count);
    return 0;
}

/* ── hep-ci cancel ───────────────────────────────────────────────────────── */
static int cmd_cancel(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "cancel: Usage: hep-ci cancel <run-id>\n"); return 1;
    }
    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);

    ci_run run;
    if (run_load(ci_dir, argv[1], &run) != 0) {
        fprintf(stderr, "cancel: run '%s' not found\n", argv[1]); return 1;
    }
    if (run.status != CI_STATUS_RUNNING) {
        printf("cancel: run '%s' is not running (status: %s)\n",
               argv[1], ci_status_str(run.status));
        return 0;
    }

    /* write a cancel marker file */
    char cancel_path[CI_MAX_PATH];
    snprintf(cancel_path, sizeof(cancel_path),
             "%s/cancel_%s", ci_dir, argv[1]);
    FILE *f = fopen(cancel_path, "w");
    if (f) { fprintf(f, "cancel\n"); fclose(f); }

    run.status   = CI_STATUS_CANCELLED;
    run.finished = time(NULL);
    run_save(ci_dir, &run);

    printf("cancel: run '%s' marked for cancellation\n", argv[1]);
    return 0;
}

/* ── hep-ci clean ────────────────────────────────────────────────────────── */
static int cmd_clean(int argc, char **argv) {
    int keep = 20;
    if (argc >= 2) keep = atoi(argv[1]);

    char repo_root[CI_MAX_PATH]; find_repo_root(repo_root, sizeof(repo_root));
    char ci_dir[CI_MAX_PATH];
    snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);

    ci_run *runs; int count;
    run_list(ci_dir, &runs, &count);

    if (count <= keep) {
        printf("clean: only %d runs, nothing to clean (keeping %d)\n",
               count, keep);
        run_list_free(runs, count); return 0;
    }

    int deleted = 0;
    for (int i = 0; i < count - keep; i++) {
        /* delete log file */
        if (runs[i].log_path[0])
            remove(runs[i].log_path);
        deleted++;
    }

    /* rewrite runs.log keeping only last 'keep' unique runs */
    char runs_path[CI_MAX_PATH];
    snprintf(runs_path, sizeof(runs_path), "%s/runs.log", ci_dir);
    FILE *f = fopen(runs_path, "w");
    if (f) {
        for (int i = count - keep; i < count; i++) {
            char line[CI_MAX_STR * 4];
            run_to_line:; /* label trick */
            /* inline the run_to_line logic */
            fprintf(f, "%s|%s|%s|%s|%s|%ld|%ld|%d|%s\n",
                    runs[i].run_id, runs[i].pipeline,
                    runs[i].commit, runs[i].branch,
                    runs[i].triggered_by,
                    (long)runs[i].started, (long)runs[i].finished,
                    (int)runs[i].status, runs[i].log_path);
        }
        fclose(f);
    }

    printf("clean: removed %d old runs, kept %d\n", deleted, keep);
    run_list_free(runs, count);
    return 0;
}

/* ── hep-ci serve ────────────────────────────────────────────────────────── */
/* listen for webhook pushes from hep-server, trigger pipeline runs */

static void handle_webhook(int fd, const char *repos_dir) {
    /* read HTTP request */
    char buf[8192]; int n = (int)read(fd, buf, sizeof(buf)-1);
    if (n <= 0) { close(fd); return; }
    buf[n] = '\0';

    /* parse repo name and commit from webhook body
     * hep-server sends: POST / with body: repo=name&commit=sha&branch=main */
    char repo_name[256] = {0}, commit[41] = {0}, branch[256] = "main";
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) body = strstr(buf, "\n\n");
    if (body) {
        body += (body[0] == '\r') ? 4 : 2;
        /* parse URL-encoded body */
        char *rp = strstr(body, "repo=");
        if (rp) sscanf(rp+5, "%255[^&\n]", repo_name);
        char *cp = strstr(body, "commit=");
        if (cp) sscanf(cp+7, "%40[^&\n]", commit);
        char *bp = strstr(body, "branch=");
        if (bp) sscanf(bp+7, "%255[^&\n]", branch);
    }

    /* send 200 OK immediately */
    const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    write(fd, ok, strlen(ok));
    close(fd);

    if (!repo_name[0]) return;

    char repo_root[CI_MAX_PATH];
    snprintf(repo_root, sizeof(repo_root), "%s/%s", repos_dir, repo_name);

    char yml_path[CI_MAX_PATH];
    snprintf(yml_path, sizeof(yml_path), "%s/%s", repo_root, CI_YML);

    struct stat st;
    if (stat(yml_path, &st) != 0) {
        printf("serve: repo '%s' has no %s — skipping\n",
               repo_name, CI_YML);
        return;
    }

    /* fork and run pipeline */
    pid_t pid = fork();
    if (pid == 0) {
        /* child */
        ci_pipeline p; char err[256];
        if (pipeline_parse(yml_path, &p, err, sizeof(err)) != 0) {
            fprintf(stderr, "serve: parse error for %s: %s\n",
                    repo_name, err);
            exit(1);
        }

        char ci_dir[CI_MAX_PATH];
        snprintf(ci_dir, sizeof(ci_dir), "%s/%s", repo_root, CI_DIR);
        mkdir(ci_dir, 0755);
        char logs_dir[CI_MAX_PATH];
        snprintf(logs_dir, sizeof(logs_dir), "%s/logs", ci_dir);
        mkdir(logs_dir, 0755);

        char run_id[64]; make_run_id(run_id);
        char log_path[CI_MAX_PATH];
        snprintf(log_path, sizeof(log_path), "%s/%s.log", logs_dir, run_id);

        ci_run run = {0};
        strncpy(run.run_id, run_id, 63);
        strncpy(run.pipeline, p.name, CI_MAX_STR-1);
        strncpy(run.commit, commit[0] ? commit : "none", 40);
        strncpy(run.branch, branch, 255);
        strncpy(run.triggered_by, "push", 127);
        run.started = time(NULL);
        run.status  = CI_STATUS_RUNNING;
        strncpy(run.log_path, log_path, CI_MAX_STR-1);
        run_save(ci_dir, &run);

        printf("serve: running pipeline for '%s' (run %s)\n",
               repo_name, run_id);

        int rc = pipeline_run(&p, repo_root, log_path, run_id);

        run.finished = time(NULL);
        run.status   = rc == 0 ? CI_STATUS_PASS : CI_STATUS_FAIL;
        run_save(ci_dir, &run);

        printf("serve: run %s %s\n", run_id,
               rc == 0 ? "PASS" : "FAIL");
        exit(rc);
    }
    /* parent continues accepting */
}

static int cmd_serve(int argc, char **argv) {
    int port = CI_SERVE_PORT;
    const char *repos_dir = "./repos";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i+1 < argc)
            port = atoi(argv[++i]);
        else if (strcmp(argv[i], "-d") == 0 && i+1 < argc)
            repos_dir = argv[++i];
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("serve: bind"); return 1;
    }
    listen(srv, 32);

    printf("hep-ci serve listening on :%d\n", port);
    printf("repos dir: %s\n", repos_dir);
    printf("waiting for push webhooks from hep-server...\n\n");
    fflush(stdout);

    while (1) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        pid_t pid = fork();
        if (pid == 0) {
            close(srv);
            handle_webhook(fd, repos_dir);
            exit(0);
        }
        close(fd);
    }
    return 0;
}

/* ═════════════════════════════════════════════════════════════════════════
 * DISPATCH + HELP
 * ═════════════════════════════════════════════════════════════════════════ */

static void usage(void) {
    printf(
        "hep-ci — continuous integration for the hep ecosystem\n\n"
        "usage: hep-ci <command> [options]\n\n"
        "commands:\n"
        "  init              create .hep-ci.yml pipeline file\n"
        "  run [file]        run pipeline locally now\n"
        "  status [n]        show last N runs (default 10)\n"
        "  logs [run-id]     show output of a run (default: last)\n"
        "  watch             tail output of currently running job\n"
        "  serve [-p port]   start CI daemon for push-triggered runs\n"
        "  cancel <run-id>   cancel a running job\n"
        "  history           full run history with stats\n"
        "  clean [keep]      delete old logs, keep N most recent (default 20)\n\n"
        "pipeline file: .hep-ci.yml\n\n"
        "example .hep-ci.yml:\n"
        "  name: build\n"
        "  on: push\n"
        "  jobs:\n"
        "    build:\n"
        "      steps:\n"
        "        - name: compile\n"
        "          run: gcc -o app main.c\n"
        "        - name: test\n"
        "          run: ./app --test\n\n"
        "ecosystem:\n"
        "  hep         version control  (92 commands)\n"
        "  hep-server  repo server      (git HTTP + web UI)\n"
        "  hep-ci      this\n"
    );
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 0; }

    const char *cmd = argv[1];

    if (!strcmp(cmd, "init"))    return cmd_init(argc-1,    argv+1);
    if (!strcmp(cmd, "run"))     return cmd_run(argc-1,     argv+1);
    if (!strcmp(cmd, "status"))  return cmd_status(argc-1,  argv+1);
    if (!strcmp(cmd, "logs"))    return cmd_logs(argc-1,    argv+1);
    if (!strcmp(cmd, "watch"))   return cmd_watch(argc-1,   argv+1);
    if (!strcmp(cmd, "serve"))   return cmd_serve(argc-1,   argv+1);
    if (!strcmp(cmd, "cancel"))  return cmd_cancel(argc-1,  argv+1);
    if (!strcmp(cmd, "history")) return cmd_history(argc-1, argv+1);
    if (!strcmp(cmd, "clean"))   return cmd_clean(argc-1,   argv+1);

    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { usage(); return 0; }

    fprintf(stderr, "hep-ci: unknown command '%s'\n"
                    "run 'hep-ci --help' for usage\n", cmd);
    return 1;
}
