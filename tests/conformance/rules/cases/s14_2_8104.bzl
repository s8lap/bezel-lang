//! case: s14_2_8104
//! doc: spec
//! section: 14.2
//! title: string does not conform to IEnumerable<int> at an explicit call site
//! rule: `where T: IFoo` | Non-nullable types conforming to `IFoo`
//! cites: 14.2
//! expect: reject
//! diagnostic: where
//! notes: Pins #8796. The standard library declares `public class String:
//!  IEnumerable<char>, ...`, so string conforms to IEnumerable<char> only. Twin:
//!  s14_2_8105.
module Conf;
class P
  public static method F<S>(a: S): int where S: IEnumerable<int> => 1;
  public static method Main(): int
    Console.WriteLine(P.F<string>("x"));
    return 0;
  end Main;
end P;
