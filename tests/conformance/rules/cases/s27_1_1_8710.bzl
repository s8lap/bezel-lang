//! case: s27_1_1_8710
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value returned at a nullable plain return slot is rejected
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: reject
//! diagnostic: modifier slots
//! notes: Modifier slots must match exactly, and a slot's nullability relates only in the slot's
//!   direction, not the modifier: `() -> owned K` returned at `() -> K?` differs in the
//!   `owned` modifier. Rejected at the `return`. Twin: s27_1_1_8711, which returns it at `()
//!   -> owned K?`.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Make(): () -> K?
    var f: () -> owned K := lambda() => new K(5);
    return f;
  end Make;
  public static method Main(): int => 0;
end Program;
