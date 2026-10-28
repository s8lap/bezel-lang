# r0131 probes — the trust-admission report on the r35 release compiler

`run_all.sh` compiles every `[a-e]NN_*.bzl` with `bezelc 0.0.0-f6863e62` twice (`--checked-disposal --report-trust-admissions`, then `--checked-disposal` alone), `-c` for library-shaped probes and a full link + run for those with a `Main`, and writes `results.md` (verdict, count of `trust admission recorded` lines, first diagnostics, run output). Per-probe logs are under `out/`.

| prefix | family | what it covers |
|---|---|---|
| `a01`–`a13` | field-held (§28.3 store rule) | parameter, `scoped` twin, field path through a parameter, bare field vs `this.field` spelling, property store, primary-constructor capture, field initializer, function-value invocation result, interface type, controls |
| `b01`–`b16` | collection-held (§28.11) | `Add`/`Insert`/`Push`/`Enqueue`/`AddFirst`/indexer-set/`Dictionary.Add`, unclassified receiver, `OwningDictionary` key, purely local collection, 0157's transfer entry and its quiet twins, fresh store (0207), comparer-held (blocked), parameter-held and `owned`-parameter receivers, `AddRange` |
| `c01`–`c13` | frame-held (§30.1, §30.5) | `Task.FireAndForget` over a receiver / over a static call's argument / over an instance call's argument / over an async lambda's capture; task stored in a field / owning collection; bound-not-awaited; Pattern A plain operand; `return await` over an owned field; quiet controls (awaited, mint-announcing return, `TaskScope`) |
| `d01`–`d04` | §28.14 residuals | form-3 registration residual, `scoped` field quiescence, form-2 and scoped-construction quiet twins |
| `e01`–`e09` | exclusions and candidates | K4.2 conditional kill; the #7434 slot-kill-by-move transfer (cross-body and one-body); §22.12 static warning; the #4602 free-at-Dispose run probe (e05); the temporary-into-storing-callee dangle (e06); the replacing-`move` parse (e09) |

Measured summary (2026-10-28): the mode reports the plain-parameter / `this.`-spelled field store, the collection store (classified and unclassified receivers) and `Task.FireAndForget` over a member-call receiver; it misses argument-held frame sources (the spec's §30.1 example), every other frame-held form, Family D, the comparer-held clause, the 0157 transfer entry, bare-field-spelled sources, primary-constructor captures, field initializers, function-value invocation results, the `OwningDictionary` key and `AddRange`; and it over-reports a purely local collection store. The corpus sweep (`../work/sweep/`) adds seven over-reports of non-borrow operands among 50 lines over 3,864 programs.
