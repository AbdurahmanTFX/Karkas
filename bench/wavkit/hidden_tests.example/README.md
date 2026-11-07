# Example of hidden acceptance tests (copy OUTSIDE the repository before measuring)

    python3 run.py --wavkit <path to build/wavkit> --scenario s1|s2|s3

Prints `PASS <name>` / `FAIL <name>: <why>` per test and a last line `hidden <pass>/<total>`.
The tests only use the command line and the file formats fixed in spec.md, so they work for
any internal structure. S3 runs the S1 suite (the injected bugs break it); S2 adds the five
extensions of s2_tasks.txt.
