//! case: l9_6_0073
//! doc: stdlib
//! section: 9.6
//! title: Trim with an Array<string> chars argument matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: An Array<string> is an IEnumerable<string>; the elements are not flattened into chars.
module Conf;

public class Program
  public static method Main()
    var a: Array<string> := ["a"];
    Console.WriteLine("ab".Trim(a));
  end Main;
end Program;
