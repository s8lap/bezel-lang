//! case: s14_2_8107
//! doc: spec
//! section: 14.2
//! title: a type-parameter bound is satisfied by a conforming substitution at a call site
//! rule: `where T: U` (U a type parameter) | Non-nullable subtypes of the substitution for
//!  `U`
//! cites: 14.2
//! expect: run
//! output: 1
//!   1
//! notes: Admitting twin of s14_2_8106 (#8796): string conforms to IEnumerable<char>, int
//!  to IComparable<int> (standard-library declarations).
module Conf;
class P
  public static method F<T, S>(a: S): int where S: T => 1;
  public static method Main(): int
    Console.WriteLine(P.F<IEnumerable<char>, string>("x"));
    Console.WriteLine(P.F<IComparable<int>, int>(3));
    return 0;
  end Main;
end P;
