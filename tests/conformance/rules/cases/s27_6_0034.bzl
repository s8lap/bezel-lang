//! case: s27_6_0034
//! doc: spec
//! section: 27.6
//! title: a method reference to a concrete Array's Contains member is admitted
//! rule: "Method references work for static methods, instance methods (the instance
//!   is captured at formation — by reference for class- or interface-typed
//!   receivers, by copy for value-shaped receivers)" (§27.6, v0.340). The head `a`
//!   is a concrete `Array<int>` — a class type, so the capture is by reference and
//!   the formation is admitted. KNOWN DIVERGENCE (#7093): the compiler rejects the
//!   formation in codegen with the #2470/#2492 fence ("a bare reference to
//!   `Contains` is not yet supported on `Array<T>`"), while the same reference over
//!   the interface head is admitted; the two receivers disagree and no spec
//!   sentence rejects the formation.
//! cites: 27.6
//! expect: accept
//! issue: 7093
module Conf;

import System;

public class Demo
  public method Go(): bool
    var a: Array<int> := [1, 2, 3];
    var f: (int) -> bool := a.Contains;
    return f(2);
  end Go;
end Demo;
