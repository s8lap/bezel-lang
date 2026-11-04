//! case: s22_15_0033
//! doc: spec
//! section: 22.15
//! title: a pure body may not call Console.WriteLine, a stdlib member declared without pure
//! rule: The compiler rejects a `pure` member whose body violates the rules of
//!   this section, and rejects, inside a pure body, any call whose
//!   statically resolved declaration is not pure.
//! cites: 22.15
//! expect: reject
//! diagnostic: pure
//! issue: 7453
module Conf;

public class Ops
  public static pure method Show(s: string): int
    Console.WriteLine(s);
    return 0;
  end Show;
end Ops;
