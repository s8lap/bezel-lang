// RUN: %bezelc -c -o %t.o %s
//
// §35.4.3 / §9.7 / §30.1 (v0.226, issue #2183): the runtime-managed axis of the
// over-rejection control. `Opt<string>` substitutes a runtime-managed payload;
// runtime-managed values have unbounded lifetime by construction (§9.7), so they
// are classification-neutral and the choice stays value-like — a legal `async`
// parameter. (`ReadOnlyStringSpan` etc. are excluded from spans for the same
// reason.)

module M; choice Opt<T> Some(v: T), None; end Opt; static class F public static async method Ok(x: Opt<string>): Task<int> return 1; end Ok; end F; class C public static method Main(): int return 0; end Main; end C;
