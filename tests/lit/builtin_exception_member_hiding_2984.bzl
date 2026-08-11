// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// (`%not` = LLVM's exit-status inverter; the compile is expected to fail —
// see tests/lit/README.md for why every reject test wraps `bezelc` in `not`.)
//
// Issue #2984 / spec §12.2 (Hiding is not allowed) / §31.4 (Standard exception
// types) / §12.1, v0.240. The ACCEPT side — every shape that must keep
// compiling — is pinned with its values in
// tests/codegen/builtin_exception_member_hiding_2984_test.cpp.
//
// §12.2, verbatim: "Declaring a member in a derived class with the same name as
// **any** inherited member **visible at the derived class's declaration site**
// is a compile error, unless the declaration is an `override` of a virtual
// member with a matching signature per §12.1 ... To add a new member, choose a
// different name." Its blocked-name set: "`public` and `protected` base members
// always block". §31.4 declares `Message`, `StackTrace` and `CauseChain` as
// `public readonly property` on `Exception`, and `ParamName` on
// `ArgumentException`; none is `virtual`, so no `override` escape exists.
//
// THE DEFECT — the SILENT direction of the same absence #2983 hit loudly. The
// §12.2 check seeds its inherited-member frontier from `TypeSymbol::baseTypes`,
// and a BUILT-IN exception base is never in that collection (it has no
// TypeSymbol at all), so the inherited set was EMPTY for every class deriving
// from one and the rule simply did not run. A negative-shaped check fed by a
// collection that never held the population fails by ACCEPTING.
//
// What the accepted program did, MEASURED on 0dac23f — `class ExOwn: Exception`
// with `public property Message: string => _m;` and `base("header")` compiled
// with ZERO diagnostics, and the SAME OBJECT then answered `Message` two
// different ways, both through the subclass's own type:
//     this.Message  ->  shadow    (the declared property)
//     p.Message     ->  header    (the inherited §31.4 slot at byte offset 8)
// The object really does carry two `Message`s, because `classStruct` inserts
// the §31.4 header for any class `sema::builtinExceptionBaseOf` accepts. That
// is precisely the shadowing hazard §12.2's recorded motivation names.
//
// THE MESSAGE NAMES THE BUILT-IN THAT DECLARES THE MEMBER, not the class's own
// base: §31.4 puts `StackTrace` on `Exception` and `ParamName` on
// `ArgumentException`, so `Deep` below (two steps under ArgumentException) must
// be told `Exception` for `StackTrace`. Naming the base for both would state
// something false about the user's hierarchy — the #2983 defect in miniature —
// so the walk that finds the member is what reports where it came from.
module M2984;

class ExMsg: Exception
  private _m: string := "shadow";
  public property Message: string => _m;
end ExMsg;

class ExSt: Exception
  public property StackTrace: string => "own";
end ExSt;

// Not just properties: any member kind with the name is blocked. §12.2 "This
// holds even when the signatures differ".
class ExCc: Exception
  public method CauseChain(): int  return 1; end CauseChain;
end ExCc;

// A FIELD, and on an ArgumentException subclass — the §31.4 level that declares
// `ParamName`.
class ExPn: ArgumentException
  public constructor() base("m"); end constructor;
  public ParamName: string := "own";
end ExPn;

// No `override` escape: §31.4's members are not `virtual`, so the `override`
// spelling is rejected too — and the message says why rather than suggesting it.
class ExOv: Exception
  public override property Message: string => "x";
end ExOv;

// Two levels down, through a USER class, from `ArgumentException`. The
// inherited-name set must reach the built-in across the user step, and the
// message must name `Exception` (which declares StackTrace), NOT the class's
// own built-in base `ArgumentException`.
class Deep: ExPn
  public StackTrace: string := "deep";
end Deep;

class C
  public static method Main(): int
    return 0;
  end Main;
end C;

// bezelc emits diagnostics as `file:line:col: message` with no "error:"
// severity word, so anchor on the `:line:col:` shape plus the message text.
// The line/column patterns exclude 0, so a regression to `0:0` fails the cell.

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: member `Message` hides the inherited `Message` declared by the built-in `Exception` (§31.4)
// CHECK-SAME: hiding is forbidden
// CHECK-SAME: not `virtual`, so `override` is not available either
// CHECK-SAME: (§12.2)

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: member `StackTrace` hides the inherited `StackTrace` declared by the built-in `Exception` (§31.4)

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: member `CauseChain` hides the inherited `CauseChain` declared by the built-in `Exception` (§31.4)

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: member `ParamName` hides the inherited `ParamName` declared by the built-in `ArgumentException` (§31.4)

// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: member `Message` hides the inherited `Message` declared by the built-in `Exception` (§31.4)

// `Deep`'s own built-in base is `ArgumentException`, but `StackTrace` is
// declared by `Exception` — and that is what the message must say.
// CHECK: :{{[1-9][0-9]*}}:{{[1-9][0-9]*}}: member `StackTrace` hides the inherited `StackTrace` declared by the built-in `Exception` (§31.4)
