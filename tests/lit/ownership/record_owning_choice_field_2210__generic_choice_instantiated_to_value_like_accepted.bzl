// RUN: %bezelc -c -o %t.o %s
//
// §9.7 / §35.4.1 (v0.226, issue #2210, #2183 family): the ACCEPT
// (instantiation-re-check) control. A generic choice `Opt<T>` whose
// argument is a NON-OWNING type (`int`) instantiates to a value-like
// choice (§35.4.3) and the record field declaration is admitted. The
// §35.4 classifier at the declaration site sees a bare type parameter
// `T` and falls back to the conservative reading (a bare type parameter
// is not classifiable as move-only, #2183, so the verdict is NOT Owning).
// The instantiation-site re-check is the existing #2183 path's
// responsibility and runs at each use of the recorded-classtype pair.

module M; choice Opt<T> Some(v: T), None; end Opt; record HolderO(K: Opt<int>); class C public static method Main(): int var h := new HolderO(Opt<int>.Some(42)); return 0; end Main; end C;
