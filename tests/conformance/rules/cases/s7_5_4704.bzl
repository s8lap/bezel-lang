//! case: s7_5_4704
//! doc: spec
//! section: 7.5
//! title: ?.Sum() through the list and enumerable interfaces performs the sum and is int?
//! rule: The result type is always nullable:
//! cites: 5
//! expect: run
//! output: 24
//!   24
//!   24
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [7, 8, 9];
    var il: IList<int>? := a;
    var rol: IReadOnlyList<int>? := a;
    var e: IEnumerable<int>? := a;
    Console.WriteLine(il?.Sum() ?? -1);
    Console.WriteLine(rol?.Sum() ?? -1);
    Console.WriteLine(e?.Sum() ?? -1);
  end Main;
end Program;
