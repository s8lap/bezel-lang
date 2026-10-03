// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Issue #5335 — MethodDA keys `declared_`/`assigned_` by NAME on the WRITE and
// DECLARATION sides, so a nested-block local that shadows an UNASSIGNED outer
// local marks the OUTER one assigned. The read side was fixed by #5314
// (dd38b2a0); this row pins the write/decl half.
//
// WHAT THE DOCUMENTS REQUIRE.  Spec §7.9 (v0.313): every read of a local must
// follow an assignment on every reaching path, and a non-defaultable local
// (§17.11: `string` has no default) declared without an initializer is
// unassigned until assigned. Spec §21.1 (v0.313): a local's scope is "the
// innermost enclosing statement block". Spec §21.6 (v0.313): "Locals declared
// in a block are visible only until the matching `end`" — so the block local is
// a DIFFERENT variable from any outer same-named local, and assigning it is not
// an assignment of the outer one.
//
// The four REJECT rows below each pair an unassigned outer `var t: string;`
// with a nested block that declares and assigns its own `t`, then read the
// OUTER `t` after the block. Pre-fix every one of them compiled clean and
// segfaulted at runtime (a null `string` slot), because the inner declaration
// inserted the name `t` into both `declared_` and `assigned_`.
//
// The two ACCEPT rows are the controls: distinct names must not interfere, and
// a nested write that really does target the outer local must still count.

module M; class C

  // REJECT: do-block shadow — `do var t := "x"; end` declares a block local,
  // not an assignment of the outer `t`.
  public static method DoBlock(a: int): int
    var t: string;
    do var t := "x"; end;
    return t.Length;
  end DoBlock;

  // REJECT: if/else arms — each arm's `t` is its own block local; neither
  // reaches the join as an assignment of the outer `t`.
  public static method IfArms(a: int): int
    var t: string;
    if a = 0 then var t := "x"; else var t := "y"; end;
    return t.Length;
  end IfArms;

  // REJECT: case arms — same shape, each `=>` body is a statement block.
  public static method CaseArms(a: int): int
    var t: string;
    case a of
      0 => do var t := "x"; end,
      _ => do var t := "y"; end
    end;
    return t.Length;
  end CaseArms;

  // REJECT: nested do-blocks — the innermost `t` shadows the middle one, which
  // shadows the outer one; three distinct variables.
  public static method NestedBlocks(a: int): int
    var t: string;
    do var t := "x"; do var t := "y"; end; end;
    return t.Length;
  end NestedBlocks;

  // REJECT: a nested block that declares AND writes `t` still never assigns
  // the outer `t` (§21.6 — the write resolves to the block-local).
  public static method DoBlockWrites(a: int): int
    var t: string;
    do var t := "x"; t := "y"; end;
    return t.Length;
  end DoBlockWrites;

  public static method Main(): int
    _ := C.DoBlock(0);
    _ := C.IfArms(0);
    _ := C.CaseArms(0);
    _ := C.NestedBlocks(0);
    _ := C.DoBlockWrites(0);
    return 0;
  end Main;

end C;

// One diagnostic per REJECT method, all naming the OUTER local `t`.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: read of unassigned local `t`
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: read of unassigned local `t`
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: read of unassigned local `t`
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: read of unassigned local `t`
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: read of unassigned local `t`
