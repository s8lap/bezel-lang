// RUN: %bezelc -c -o %t.o %s
//
// Admitting twin of `pure_ctor_chain_primary_8163_base_ctor_reject.bzl`
// (§22.15, v0.355): the base constructor is `pure`, so the chain is pure.

module M;
class PureBase : Exception
  public pure constructor(m: string) base(m);
  end constructor;
end PureBase;
class E(m: string): PureBase(m)
  public pure constructor() this("d"); end constructor;
end E;
