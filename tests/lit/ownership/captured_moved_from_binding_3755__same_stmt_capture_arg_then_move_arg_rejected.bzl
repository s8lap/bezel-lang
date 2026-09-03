// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// §28.3 / §22.5 (v0.286, issue #3755, review follow-up): the REVERSE argument
// order — the capturing lambda literal is written BEFORE the `move`, so `p` is
// still Owned where the literal is evaluated. It is nonetheless a
// heap-use-after-free (measured on e367a45 under `--sanitize=address`,
// identical signature to the other order), because the callee invokes the
// callback AFTER consuming the transferred value: §22.5 fixes when the
// argument EXPRESSIONS run, not when the closure is INVOKED.
//
// On its face this is §28.3's consume-site freeze — "A borrow is *live* at a
// consume site when some path from the site reaches a later use of it — …
// through a closure value that is itself still live" — but that check's
// implementation sees only closures held by LOCALS, so an argument-position
// closure temporary escapes it. This row is what makes the rule
// order-INdependent. // per §28.3, §22.5 (v0.286)

module M; import System; class R private v: int := 5; public method V(): int return this.v; end V; destructor end destructor; end R; class C public static method GoR(f: () -> int, x: owned R): int x.Dispose(); return f(); end GoR; public static method Run(): int var p := new R(); return GoR(lambda(): int => p.V(), move p); end Run; public static method Main(): int Console.WriteLine(Run().ToString()); return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: closure captures `p` whose owned value is transferred away by this same statement (§28.3 / §22.5)
