//! case: l9_6_0077
//! doc: stdlib
//! section: 9.6
//! title: Trim with a lazy pipeline of ints matches no declared form
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: reject
//! diagnostic: Trim
//! issue: 7521
//! notes: Lazy shape of the element-type rule.
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1];
    Console.WriteLine("ab".Trim(a.Where(lambda(x) => x > 0)));
  end Main;
end Program;
