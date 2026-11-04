//! case: l9_6_0064
//! doc: stdlib
//! section: 9.6
//! title: Trim admits a Set<char> as its chars argument and leaves the set intact
//! rule: public pure method Trim(chars: IEnumerable<char>): string;
//! cites: 9.6, 5.1
//! expect: run
//! output: 0
//!   1
//! issue: 7521
//! notes: Set<T> is an ICollection<T>, which extends IReadOnlyCollection<T> : IEnumerable<T>, so Set<char> conforms to IEnumerable<char> (5.1). The removal rule is not stated by stdlib 9.6, so the receiver is the empty string: the case pins admission and that the source is borrowed, not the trimming.
module Conf;

public class Program
  public static method Main()
    using var s := new Set<char>();
    s.Add('a');
    Console.WriteLine("".Trim(s).Length);
    Console.WriteLine(s.Count);
  end Main;
end Program;
