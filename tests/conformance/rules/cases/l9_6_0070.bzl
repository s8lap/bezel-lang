//! case: l9_6_0070
//! doc: stdlib
//! section: 9.6
//! title: Trim admits a lazy Where pipeline over chars as its chars argument and leaves its source intact
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: run
//! output: 0
//!   2
//! issue: 7521
//! notes: Where yields an IEnumerable<char> over an Array<char> source (stdlib 5). The removal rule is not stated by stdlib 9.6, so the receiver is the empty string: the case pins admission and that the source is borrowed, not the trimming.
module Conf;

public class Program
  public static method Main()
    var a: Array<char> := ['a', 'b'];
    Console.WriteLine("".Trim(a.Where(lambda(c) => c = 'a')).Length);
    Console.WriteLine(a.Count);
  end Main;
end Program;
