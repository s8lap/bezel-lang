// §22.12 rule 2 form (iii) (v0.221 ruling / #1393, spec v0.228): a mutable
// static FIELD of primitive value type (§3.1) is admitted with a
// declaration-site warning, not silently (the under-enforcement issue #1393
// reports) and not rejected (the category the compiler must NOT regress to
// treating as an error).
//
// ACCEPT-WITH-WARNING control: bezelc -c must exit zero (the program is
// legal), but it DOES emit a diagnostic — pinned below so the warning stays
// observed.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck %s

module M; class App public static _count: int := 0; public static method Main(): int => _count; end App;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}:{{ ?warning: ?}} mutable static field `_count` has type `int` — a primitive/enum value static is admitted only with a declaration-site warning (§22.12 rule 2 form (iii)): the storage is process-global and reachable from every domain; confine every read and write to one domain, or use a synchronized spelling (`AtomicInteger<T>`/`Atomic<T>` for the cell-sized primitives, a `Mutex`/`Locked<T>` cell for `decimal` and enums)
// CHECK-SAME: (§22.12 rule 2, v0.221, #1393)
