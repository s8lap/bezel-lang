//! case: s14_2_8105
//! doc: spec
//! section: 14.2
//! title: string conforms to IEnumerable<char> at an explicit call site
//! rule: `where T: IFoo` | Non-nullable types conforming to `IFoo`
//! cites: 14.2
//! expect: run
//! output: 1
//! notes: Admitting twin of s14_2_8104 (#8796): the standard library declares `public class
//!  String: IEnumerable<char>`.
module Conf;
class P
  public static method F<S>(a: S): int where S: IEnumerable<char> => 1;
  public static method Main(): int
    Console.WriteLine(P.F<string>("x"));
    return 0;
  end Main;
end P;
