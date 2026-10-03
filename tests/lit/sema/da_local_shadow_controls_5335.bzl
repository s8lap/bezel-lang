// RUN: %bezelc -c %s
//
// Issue #5335 — controls for the write/decl symbol keying in
// src/sema/DefiniteAssignment.cpp. Companion to
// tests/lit/sema/da_local_shadow_write_decl_keyed_5335.bzl, which pins the
// five REJECT shapes. These rows must keep compiling: keying `declared_` /
// `assigned_` by the resolved LocalSymbol must not reject a program whose
// locals are genuinely assigned.
//
// Spec §7.9 (v0.313): a read of a local must follow an assignment on every
// reaching path. Spec §21.1 / §21.6 (v0.313): a block local is a different
// variable from an outer same-named local — which cuts BOTH ways: it must not
// be treated as an assignment of the outer one (the REJECT rows), and a write
// that genuinely targets the outer local must still discharge its §7.9
// obligation.
//
// Row 1: distinct names — a nested block local `u` must not affect `t`.
// Row 2: the outer local is assigned BEFORE the block, so the read after it is
//         legal and its length is 2.
// Row 3: the nested write is in the ENCLOSING scope after the block ends, so it
//         does resolve to the outer `t` and discharges §7.9 for it (1).
// Row 4: an `if` that assigns the outer `t` on BOTH arms is a legal
//         all-paths assignment (1).

module M; class C

  // ACCEPT: distinct names.
  public static method DistinctNames(a: int): int
    var t := "xy";
    do var u := "z"; end;
    return t.Length;
  end DistinctNames;

  // ACCEPT: assigned before the block; the read after it is legal (2).
  public static method AssignedBefore(a: int): int
    var t: string;
    t := "xy";
    do var u := "z"; end;
    return t.Length;
  end AssignedBefore;

  // ACCEPT: the write is in the ENCLOSING scope, after the block, so it names
  // the outer `t` and discharges §7.9 for it (1).
  public static method WriteAfterBlock(a: int): int
    var t: string;
    do var u := "z"; end;
    t := "q";
    return t.Length;
  end WriteAfterBlock;

  // ACCEPT: an `if` assigning the outer `t` on both arms is an all-paths
  // assignment (1).
  public static method IfBothArms(a: int): int
    var t: string;
    if a = 0 then t := "x"; else t := "y"; end;
    return t.Length;
  end IfBothArms;

  public static method Main(): int
    Console.WriteLine(C.DistinctNames(0));
    Console.WriteLine(C.AssignedBefore(0));
    Console.WriteLine(C.WriteAfterBlock(0));
    Console.WriteLine(C.IfBothArms(0));
    return 0;
  end Main;

end C;
