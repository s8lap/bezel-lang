//! case: l9_6_0067
//! doc: stdlib
//! section: 9.6
//! title: Trim admits a LinkedList<char> as its chars argument and leaves the list intact
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: run
//! output: 0
//!   1
//! issue: 7521
//! notes: LinkedList<T> is an ICollection<T> : IEnumerable<T>, so LinkedList<char> conforms to IEnumerable<char>. The removal rule is not stated by stdlib 9.6, so the receiver is the empty string: the case pins admission and that the source is borrowed, not the trimming.
module Conf;

public class Program
  public static method Main()
    using var l := new LinkedList<char>();
    l.Add('a');
    Console.WriteLine("".Trim(l).Length);
    Console.WriteLine(l.Count);
  end Main;
end Program;
