//! case: s27_1_1_8708
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value flowing through a conditional into a plain return slot is
//!   rejected
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5, 19.2
//! expect: reject
//! diagnostic: modifier slots
//! notes: Modifier slots must match exactly. Both arms of the conditional are the `() -> owned K`
//!   local `f`; the initializer's target is the plain `() -> K`. Rejected at the initializer.
//!   Twin: s27_1_1_8709.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Main(): int
    var f: () -> owned K := lambda() => new K(5);
    var h: () -> K := (if true then f else f);
    return 0;
  end Main;
end Program;
