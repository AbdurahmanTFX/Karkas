# 0006 — One build at a time per project

Status: implemented

All agents of a project share one build tree (`build/`), and Ninja must not run twice in the
same tree. Builds are therefore serialized by a process mutex plus `flock` on
`.karkas/build.lock`; `max_parallel_builds` > 1 has no effect until per-agent build trees
exist. Agents (model calls) still run in parallel up to `max_parallel_agents`.
