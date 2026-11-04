//! case: l9_6_0071
//! doc: stdlib
//! section: 9.6
//! title: Trim with an IEnumerable<int> chars argument matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: Element type must be char. The accepting twin is the IEnumerable<char> row.
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1];
    var e: IEnumerable<int> := a;
    Console.WriteLine("ab".Trim(e));
  end Main;
end Program;
