//! case: l9_6_0069
//! doc: stdlib
//! section: 9.6
//! title: Trim admits an IEnumerable<char> variable as its chars argument and leaves its source intact
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: run
//! output: 0
//!   1
//! issue: 7521
//! notes: The argument's static type is exactly the declared parameter type. The removal rule is not stated by stdlib 9.6, so the receiver is the empty string: the case pins admission and that the source is borrowed, not the trimming.
module Conf;

public class Program
  public static method Main()
    var a: Array<char> := ['a'];
    var e: IEnumerable<char> := a;
    Console.WriteLine("".Trim(e).Length);
    Console.WriteLine(a.Count);
  end Main;
end Program;
