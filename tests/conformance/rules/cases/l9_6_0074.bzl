//! case: l9_6_0074
//! doc: stdlib
//! section: 9.6
//! title: Trim with a Set<char?> chars argument matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1, 7.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: The element type char? is distinct from char (spec 7.1).
module Conf;

public class Program
  public static method Main()
    using var s := new Set<char?>();
    Console.WriteLine("ab".Trim(s));
  end Main;
end Program;
