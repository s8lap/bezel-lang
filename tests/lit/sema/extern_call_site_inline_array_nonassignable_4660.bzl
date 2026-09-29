// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Regression pin for fix #4660 commit 5 (§32.5 / §22.3, v0.307).
//
// The by-address FFI marshalling requires the address of the CALLER's storage
// to cross the boundary, so an inline-array argument must be one of §22.3's
// location categories. Pre-fix the compiler silently accepted
// `Fill(MakeBuffer())` — a returned inline-array VALUE has no storage location
// to take the address of, so an invalid address would have crossed.
//
// The declaration-side half (`checkExternType`, #4660 commits 1-3) gates the
// FORMAL parameter's type; this gates the call-site ARGUMENT.
//
// Lives here rather than in tests/conformance/rules/: the corpus already has
// the spec-derived case for this rule (`s32_5_0018`, which this fix moves
// fail -> pass). This file is a lane-authored regression pin, which is a
// different thing from an authored spec case — the corpus's naming regex and
// header lint both correctly rejected it as a case.

module Conf;

extern "libexample"
  method Fill(buf: [4]int): ();
end;

public static class Buffers
  public static method MakeBuffer(): [4]int
    var buf: [4]int;
    return buf;
  end MakeBuffer;

  public static method Bad()
    Fill(MakeBuffer());
  end Bad;
end Buffers;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: extern call argument 1 (`{{.*}}`) is a temporary / non-assignable expression
// CHECK-SAME: inline-array argument at the FFI boundary must be an assignable storage location
// CHECK-SAME: (§32.5 / §22.3)
