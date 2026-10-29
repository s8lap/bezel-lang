//! case: l9_6_0045
//! doc: stdlib
//! section: 9.6
//! title: Trim with an int chars argument matches no declared form
//! rule: stdlib §9.6 declares `public pure method Trim(chars: IEnumerable<char>): string;`. An int is not an
//!   IEnumerable<char>, so the argument binds no declared form. Language spec §22.6 governs overload applicability.
//! cites: 9.6, 22.6
//! expect: reject
//! diagnostic: Trim
//! notes: the twin is l9_6_0043, which differs by one legal edit of the argument from int to string.
module Conf;

public class Program
  public static method Main()
    Console.WriteLine("ab".Trim(5));
  end Main;
end Program;
