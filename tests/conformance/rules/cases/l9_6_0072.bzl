//! case: l9_6_0072
//! doc: stdlib
//! section: 9.6
//! title: Trim with a Set<int> chars argument matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: Element type must be char; the twin is the Set<char> row.
module Conf;

public class Program
  public static method Main()
    using var s := new Set<int>();
    Console.WriteLine("ab".Trim(s));
  end Main;
end Program;
