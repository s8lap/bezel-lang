// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #2208 (§35.1 + §9, v0.226): a record↔choice by-value cycle is rejected at
// resolution time, before codegen. The repro on `main` a7881f3 sent
// Emitter::recordStruct into unbounded LLVM struct-body recursion (a hang /
// stack overflow / coredump); the fix walks choice case payloads through the
// same value-containment graph the record check already walks. The record-
// side check is the canonical diagnostic for record↔choice cycles; the
// choice-side check (recordsAllowed = false) deliberately does NOT fire
// because the record is on the cycle's path — exactly one diagnostic on the
// shared graph.
//
// Repro: `record Rec(K: Ch)` + `choice Ch Wrap(r: Rec), Leaf` — Ch's Wrap
// payload carries Rec by value, Rec's K field carries Ch by value; the
// struct layout is infinite and would loop codegen's Emitter::recordStruct
// into unbounded recursion without the §35.1/§9 resolution-time rejection.

module M; record Rec(K: Ch); choice Ch Wrap(r: Rec), Leaf; end Ch; class C public static method Main(): int return 0; end Main; end C;

// #7065: the same needle, written ABOVE the positive match. A negative
// directive scans only the region between the surrounding matches, so the
// copy below the match alone let this wording through when it came FIRST.
// CHECK-NOT: choice `Ch` cannot recursively contain itself
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: record `Rec` cannot recursively contain itself by value
// CHECK-NOT: choice `Ch` cannot recursively contain itself
// CHECK-SAME: (§9)
// #7065: and the same needle again, AFTER the last positive match, so the
// prohibition covers the tail of the output too — a negative directive
// scans only the region between the surrounding matches, and this one was
// bounded by the next match.
// CHECK-NOT: choice `Ch` cannot recursively contain itself
