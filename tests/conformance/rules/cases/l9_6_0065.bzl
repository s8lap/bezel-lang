//! case: l9_6_0065
//! doc: stdlib
//! section: 9.6
//! title: Trim admits a Stack<char> as its chars argument and leaves the stack intact
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: run
//! output: 0
//!   1
//! issue: 7521
//! notes: Stack<T> is an IReadOnlyCollection<T> : IEnumerable<T> (stdlib 4.x), so Stack<char> conforms to IEnumerable<char>. The removal rule is not stated by stdlib 9.6, so the receiver is the empty string: the case pins admission and that the source is borrowed, not the trimming.
module Conf;

public class Program
  public static method Main()
    using var s := new Stack<char>();
    s.Push('a');
    Console.WriteLine("".Trim(s).Length);
    Console.WriteLine(s.Count);
  end Main;
end Program;
