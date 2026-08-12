// RUN: %bezelc -c -o %t.o %s
//
// §35.7 / §35.4.3 (v0.226, issue #2183): a DIRECT classification pin for the
// instantiation-aware borrow leg. `Opt<int>` is value-like at this instantiation
// (§35.4.3), hence auto-hashable and hashable-admissible (§35.7) — like
// the stdlib `Result<int, string>`. Before the fix, the borrow leg answered the
// bare-`T` payload conservatively (borrow-bearing) at EVERY instantiation, so
// `Opt<int>` failed `where T: hashable`. This pins the correction: a concrete
// value-like generic-choice instantiation is now hashable.

module M; choice Opt<T> Some(v: T), None; end Opt; static class Box public static method Hold<T>(): int where T: hashable => 1; end Box; class C public static method Main(): int return Box.Hold<Opt<int>>(); end Main; end C;
