//! case: l9_6_0076
//! doc: stdlib
//! section: 9.6
//! title: Trim with a Dictionary<char, int> chars argument matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: A dictionary enumerates entries, not chars.
module Conf;

public class Program
  public static method Main()
    using var d := new Dictionary<char, int>();
    Console.WriteLine("ab".Trim(d));
  end Main;
end Program;
