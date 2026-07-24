// RUN: %bezelc -c -o %t.o %s
//
// §35.4.3 / §30.1 (v0.226, issue #2183): the OVER-REJECTION control. `Opt<int>`
// substitutes a value payload, so the choice is VALUE-LIKE at this instantiation
// (§35.4.3) — an ordinary copyable value with no captured borrow — and is a
// legal `async` parameter. The instantiation-aware borrow leg must reclassify
// `Opt<int>` to value-like; a naive "any generic choice is borrow-bearing"
// reading (the pre-fix conservative bare-`T` verdict) would wrongly reject this.

module M; choice Opt<T> Some(v: T), None; end Opt; static class F public static async method Ok(x: Opt<int>): Task<int> return 1; end Ok; end F; class C public static method Main(): int return 0; end Main; end C;
