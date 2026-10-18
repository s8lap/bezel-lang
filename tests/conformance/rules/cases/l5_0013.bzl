//! case: l5_0013
//! doc: stdlib
//! section: 5
//! title: ToDictionary keyed by a function type is a compile error
//! rule: ToDictionary states `where TKey: hashable` as an ordinary
//!   member-level where clause on its own type parameter, the ordinary
//!   constraint form of language spec 14.2. A function type is not hashable
//!   (spec 3.6 clause (e): a lambda equals only itself), so a key selector
//!   returning one fails the constraint at the call site -- the diagnostic
//!   names the constraint, the argument and the parameter it turns on.
//! cites: 5, 14.2, 3.6 (hashable row clause (e)), 27.7.1
//! expect: reject
//! diagnostic: where TKey: hashable
module Conf;

public class Program
  public static method Main()
    using var xs: Array<int> := [1, 2, 3];
    using var d := xs.ToDictionary(lambda(x: int): (int) -> int => Key2);
  end Main;

  public static method Key2(n: int): int => n * 2;
end Program;