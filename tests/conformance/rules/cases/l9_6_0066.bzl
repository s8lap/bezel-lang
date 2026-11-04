//! case: l9_6_0066
//! doc: stdlib
//! section: 9.6
//! title: Trim admits a Queue<char> as its chars argument and leaves the queue intact
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: run
//! output: 0
//!   1
//! issue: 7521
//! notes: Queue<T> is an IReadOnlyCollection<T> : IEnumerable<T>, so Queue<char> conforms to IEnumerable<char>. The removal rule is not stated by stdlib 9.6, so the receiver is the empty string: the case pins admission and that the source is borrowed, not the trimming.
module Conf;

public class Program
  public static method Main()
    using var q := new Queue<char>();
    q.Enqueue('a');
    Console.WriteLine("".Trim(q).Length);
    Console.WriteLine(q.Count);
  end Main;
end Program;
