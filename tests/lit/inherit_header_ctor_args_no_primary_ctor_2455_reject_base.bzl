// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// fix #2455 (§11.1 / §22.10 / §12.8, v0.235): the class-header base-
// constructor argument list `Base(99)` was silently dropped by the parser
// for any class with no primary-constructor parameter list of its own — no
// diagnostic, base constructed with defaults instead of 99. The issue's own
// confirmed repro, verbatim (LOCAL base, not cross-module).
//
// §11.1's "the base's arguments appear in parentheses after the base class
// name" and §22.10's "This replaces an explicit base(...) call" both state
// the header sugar within the primary-constructor context; §12.8 gives the
// unconditional rule for a class with neither a primary nor an explicit
// constructor — an implicit NO-ARGUMENT base() call, with no provision for
// a header argument list to feed it. Rejection is the simplest sound
// reading (CLAUDE.md rule 4): see
// tests/parser/inherit_header_ctor_args_no_primary_ctor_2455_test.cpp for
// the full analysis.

module M;

public class LocalBase
  private _v: int;
  public constructor(v: int) _v := v; end constructor;
  public property Val: int => _v;
end LocalBase;

public class LocalDerived: LocalBase(99)
end LocalDerived;

public class C
  public static method Main(): int
    using var d := new LocalDerived();
    return d.Val;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: class `LocalDerived` supplies a base-constructor argument list here, but `LocalDerived` declares no primary-constructor parameter list of its own
// CHECK-SAME: (§11.1, §22.10, v0.236)
// CHECK-SAME: (§12.8, v0.236)
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error
