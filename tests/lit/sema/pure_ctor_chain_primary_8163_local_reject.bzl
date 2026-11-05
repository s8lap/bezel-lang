// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// Diagnostic-track pin for §22.15 "Pure constructors" (v0.355): "A pure
// constructor's `base(…)`/`this(…)` chain target must itself be pure."
//
// A class PRIMARY constructor has no `ConstructorSymbol` — its parameters
// live on the `ast::TypeDecl` — so `PureBodyWalker::
// checkPureCtorChainPurity` used to resolve no chain target at all and admit
// the program SILENTLY. §12.8: a `this(...)` chain in a primary-constructor
// class ends at the primary; §22.8 step 0-2 / §22.10: the primary's
// construction runs the inheritance clause's base call and then the class's
// field initializers, for every instance of the class. So the pure
// constructor chained to a construction that runs an impure call.
//
// `Helper.Msg` is not `pure`-declared, so the primary's base-call argument
// is an impure call in a pure construction.
//
// #6620: `internal:` is codegen-only and cannot occur on a row that
// rejects in the front end; this needle is the front end's own ICE marker.
// CHECK-NOT: internal compiler error

module M;
class Helper
  public static method Msg(m: string): string
    Console.WriteLine("side effect");
    return m;
  end Msg;
end Helper;
class MyError(message: string): Exception(Helper.Msg(message))
  public pure constructor() this("default"); end constructor;
end MyError;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: call is not admitted in a pure body
// CHECK-SAME: the resolved target `Msg` is not pure-declared
// CHECK-SAME: (§22.15)
