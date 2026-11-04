//! case: l9_6_0079
//! doc: stdlib
//! section: 9.6
//! title: Trim with two arguments matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 22.5
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: Arity: Trim declares at most one parameter.
module Conf;

public class Program
  public static method Main()
    using var s := new Set<char>();
    Console.WriteLine("ab".Trim(s, s));
  end Main;
end Program;
