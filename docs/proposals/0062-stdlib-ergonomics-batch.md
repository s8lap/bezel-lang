# Proposal 0062 — Small-stdlib ergonomics batch (writer-seat sweep, 2026-07-21)

| | |
|---|---|
| **Status** | draft (owner queue) — seven trivial-to-small additions, each verified absent and un-recorded; plus an examined-and-deferred register |
| **Origin** | The 2026-07-21 writer-seat dogfooding sweep: six programs written strictly against v0.214; every item below survived decline-aware verification. |
| **Written against** | stdlib/spec v0.214 |

## 1. The additions (each independently landable)

1. **`Dictionary<K, V>.Increment(key, delta := 1)`** (value-`V` upsert) — the sweep's highest-value find: every histogram/tally pays the documented 2–3-lookup init-then-increment dance (§4.8's own text elevates it as "the supported form"). Design note the ruling must answer: `GetOrAdd`'s copy-return for value `V` is *why* no general one-liner works; an `Increment`/`AddTo` member scoped to numeric `V` (or a `Counter<K>` type) sidesteps it. |
2. **`string.IndexOf(ch: char, startIndex: int)`** — single-arg only today; the "first N fields, rest verbatim" parse shape needs it (0027's SplitSpans covers the zero-alloc general case; this is the one-call small form).
3. **`JsonDeserializer(text: string, options: JsonOptions)`** — verified missing ctor overload (§2.15); today's route is a bytes/span detour.
4. **`String.Repeat(unit: string, count: int)`** — char-only today.
5. **`String.PadCenter(width, padChar := ' ')`** — beside the existing Left/Right pair; the odd-padding bias must be picked and recorded (recommend left-biased, matching the majority precedent).
6. **`Math.DivRem(a, b): (q, r)`-shaped** — no combined form exists; belongs with 0008's integer toolkit (route there if 0008 lands first).
7. **`Environment.GetInt(name): int?` / `GetBool(name): bool?`** — typed getters over the sanctioned two-line idiom (the bind-DSL half of the wish stays declined, D-0001 territory).

**Plus one consistency defect to fix regardless of the above:** the v0.202 purity sweep left the collections inconsistent — `Array<T>.Count` is `pure readonly` while `Dictionary.Count`/`LinkedList.Count`/`OwningDictionary.Count`/`ContainsKey` are plain `readonly` (verified §4.4 vs §4.6–4.9). Filed as an issue; a `pure`-annotation pass over the collection read surface.

## 2. Examined and deferred, with reasons (recorded so they stay settled)

- **`Lazy<T>` / lazy instance fields** — real service pattern (compute-once fields), but the anti-hidden-magic posture cuts against an invisible cache (0020's regex deliberately ships with *no hidden cache* as a stated value), and v0.214's readonly-field narrowing just improved the manual `_cached: T?` idiom materially. Revisit on evidence; an owned-class-`T` `Lazy` also needs a stated once-set/dispose story (§22.13's static machinery is the precedent).
- **Raw/triple-quoted strings** — `@"…"` covers most of it; quote-dense payloads (regex, JSON-in-tests) are the residual. Revisit alongside 0019/0020 landings, which may carry their own literal ergonomics.
- **As-patterns (`name @ Pattern`)** — bindings are non-destructive borrows, so re-referencing the scrutinee is zero-cost; ceremony is cosmetic. Deferred.
- **Flags-enum niceties** (`HasFlag`, combined `ToString`) — genuinely blocked on the deferred attribute system (nothing marks which enums are bit-sets); not independently shippable.
- **`TaskScope(parentToken)` ctor and `Spawn(body, owned arg)`** — both are sugar over documented idioms (the `Register`→`Subscription` bridge; the `WorkerCtx` aggregate + `Domain.Spawn`); recorded as by-design.
- **`LinkedList` node splice/relink (`MoveToLast`)** — the un-recorded half of §4.7's node-move deferral (relink preserves node identity and moves no ownership); noted here so the eventual node-form design considers relink and move-out together, as §4.7's bundling implies.
- **Partial/derived record views (`Partial<T>`) and an overlay-merge `with`** — metaprogramming-shaped (the §18 source-generator bucket); the merge form has no clean semantics without the derived-partial type, so both defer together.
- **`Swap<T>` in the stdlib** — a three-line `ref` generic; the spec's §14.1 illustration stays an illustration.
