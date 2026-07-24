// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2208 (§35.1, v0.226): a purely choice↔choice cycle (no record in the
// cycle) is reported on the choice, not on a record. The walker
// (Resolver::recordValueContainsSelf with recordsAllowed = false) handles
// the mutual choice pair the same way §35.1 specifies for a single-cycle
// choice: by value through case payloads. Without the check, codegen's
// Emitter::choiceStruct would loop on the infinite-size struct.
//
// Repro: `choice A Wrap(b: B), Leaf` + `choice B Wrap(a: A), Leaf` —
// A's Wrap payload carries B by value, B's Wrap payload carries A by value.

module M; choice A Wrap(b: B), Leaf; end A; choice B Wrap(a: A), Leaf; end B; class C public static method Main(): int return 0; end Main; end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: choice `A` cannot recursively contain itself by value through its case payloads
// CHECK-SAME: (§35.1)
