//! case: s14_2_8106
//! doc: spec
//! section: 14.2
//! title: a type-parameter bound is checked against the substitution at a call site
//! rule: `where T: U` (U a type parameter) | Non-nullable subtypes of the substitution for
//!  `U`
//! cites: 14.2
//! expect: reject
//! diagnostic: where
//! notes: Pins #8796 (the type-parameter-bound arm). S := string against T :=
//!  IEnumerable<int>: string is not a subtype of that interface (stdlib String:
//!  IEnumerable<char>). Twin: s14_2_8107.
module Conf;
class P
  public static method F<T, S>(a: S): int where S: T => 1;
  public static method Main(): int
    Console.WriteLine(P.F<IEnumerable<int>, string>("x"));
    return 0;
  end Main;
end P;
