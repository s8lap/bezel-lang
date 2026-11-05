// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// The ADMITTING TWIN of `pure_ctor_chain_primary_8163_local_reject.bzl`
// (§22.15 "Pure constructors", v0.355): the same shape — a class with a
// primary constructor and a `pure` convenience constructor chaining
// `this(...)` to it — with a PURE base-call argument.
//
// A primary-constructor class CAN hold a pure constructor whose chain target
// is the primary, when the primary's construction content satisfies the
// pure-body rules. The reject row beside this one is rejected because of the
// IMPURITY, not because the shape is rejected outright.

module M;
class Helper
  public static pure method Msg(m: string): string
    return m;
  end Msg;
end Helper;
class MyError(message: string): Exception(Helper.Msg(message))
  public pure constructor() this("default"); end constructor;
end MyError;

// NODIAG-NOT: error
// NODIAG-NOT: §
