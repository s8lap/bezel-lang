//! case: s27_1_1_8711
//! doc: spec
//! section: 27.1.1
//! title: an owned-return function value returned at a nullable owned return slot is admitted
//! rule: **Modifier slots** (`owned`/`ref`/`out`/`out owned`/`scoped`) must match exactly
//! cites: 27.1.1, 28.3.5
//! expect: accept
//! notes: Twin of s27_1_1_8710: the return type is `() -> owned K?`, so the `owned` slots match and
//!   the nullable lift of the class slot is the covariant widening §27.1.1's second bullet
//!   admits.
module Conf;
public class K
  public readonly N: int;
  public constructor(n: int) N := n; end constructor;
end K;

public class Program
  public static method Make(): () -> owned K?
    var f: () -> owned K := lambda() => new K(5);
    return f;
  end Make;
end Program;
