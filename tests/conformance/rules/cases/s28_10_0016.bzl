//! case: s28_10_0016
//! doc: spec
//! section: 28.10
//! title: a using var over a collection literal is owned and draws no vacuity warning
//! rule: "`using var x := expr;` where `expr` is an owned producer ... is owned,
//!   additionally subject to scope-based disposal at the end of the enclosing
//!   block" (§28.10, v0.340), and "A collection literal (`[1, 2, 3]`) targeting a
//!   reference type is **Owned**" (§28.3's producers table). The literal is an
//!   owned production, so `a` is owned and its scope-end disposal is a real
//!   obligation: the "`using var` is vacuous" warning has no premise here and
//!   must not fire. KNOWN DIVERGENCE (#7092): the ownership pass does not see the
//!   producers row for a literal, so it reports the binding as having no
//!   disposal obligation.
//! cites: 28.10, 28.3
//! expect: quiet
//! issue: 7092
module Conf;

import System;

public class Demo
  public method Run()
    using var a: Array<int> := [1, 2, 3];
    a.Dispose();
  end Run;
end Demo;
