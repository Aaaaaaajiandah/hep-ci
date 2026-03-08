# hep-ci

continuous integration for the hep ecosystem. one C binary, no docker, no yaml DSL complexity — just define steps, push, watch it run.

## build

```sh
gcc -O2 -Isrc \
  src/main.c src/pipeline.c src/yaml.c \
  src/repo.c src/refs.c src/index.c src/util.c \
  src/core/sha1.c src/core/zlib_utils.c \
  src/core/odb.c src/core/blob.c src/core/tree.c src/core/commit.c \
  -lz -o hep-ci
```

## quickstart

```sh
# create pipeline file
hep-ci init

# run it locally
hep-ci run

# see what happened
hep-ci status
hep-ci logs
```

## commands

| command | does |
|---|---|
| `hep-ci init` | create `.hep-ci.yml` in current repo |
| `hep-ci run [file]` | run pipeline locally right now |
| `hep-ci status [n]` | show last N runs with pass/fail (default 10) |
| `hep-ci logs [run-id]` | show full output of a run (default: last) |
| `hep-ci watch` | tail live output of currently running job |
| `hep-ci serve [-p port] [-d dir]` | CI daemon, listens for push webhooks |
| `hep-ci cancel <run-id>` | cancel a running job |
| `hep-ci history` | full run history with pass rate and avg duration |
| `hep-ci clean [n]` | delete old logs, keep N most recent (default 20) |

## pipeline file (.hep-ci.yml)

```yaml
name: build and test

on: push

jobs:
  build:
    steps:
      - name: compile
        run: gcc -o app main.c
      - name: test
        run: ./app --test

  deploy:
    needs: build       # only runs if build passes
    workdir: /srv/app  # optional working directory
    timeout: 60        # seconds, 0 = no limit
    env:
      ENV: production
    steps:
      - name: ship it
        run: ./deploy.sh
```

## how jobs work

- jobs run in order, respecting `needs` dependencies
- if a job fails, all jobs that depend on it are skipped
- each step runs in `/bin/sh -c`
- stdout/stderr captured to `.hep-ci/logs/<run-id>.log`
- env vars `HEP_CI=1` and `HEP_REPO=<path>` always set

## serve mode (push-triggered)

run alongside hep-server to trigger CI on every push:

```sh
# terminal 1: repo server
hep-server -p 7070 -d ./repos --public

# terminal 2: CI daemon
hep-ci serve -p 7071 -d ./repos
```

hep-server sends a webhook to `localhost:7071` on every push. hep-ci picks it up, finds `.hep-ci.yml` in the repo, runs the pipeline.

## run storage

runs stored in `<repo>/.hep-ci/`:
- `runs.log` — run index (pipe-separated)
- `logs/<run-id>.log` — full output per run

## ecosystem

```
hep           version control    (92 commands)
hep-server    repo server        (git HTTP + web UI)
hep-ci        this
hep-forge     code review + issues  (existing)
```
