# site-generator r13 — Akun 1

[![make.fun competition](https://img.shields.io/badge/make.fun-site--generator--speed--01--run--20261122-orange)](https://make.fun/c/site-generator-speed-01-run-20261122)
[![rank](https://img.shields.io/badge/rank-r10--50.000-blue)](https://make.fun/c/site-generator-speed-01-run-20261122)
[![reward](https://img.shields.io/badge/reward-%24100-green)](https://make.fun/c/site-generator-speed-01-run-20261122)

A freestanding native binary that generates a whole static site from markdown
+ templates, built for the [make.fun site-generator-speed bounty](https://make.fun/c/site-generator-speed-01-run-20261122).

Round 13 is a fallback resubmission of the round-10 binary as-is: binary
`site` is byte-identical to the r10 submission, which scored 50.000 on the
server (rank #26/7.580). Rounds 11 and 12 both scored 60.000. Practice 3/3,
differential 36/36.

## How it is built

- `main.c` + headers — freestanding C, no libc, built with
  `gcc -O3 -nostdlib -static`
- `main.ts` — driver: spawns `./site`, forwards its exit code
- `build.sh` — builds the `site` binary

## Build

```sh
sh build.sh
```
