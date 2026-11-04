//! case: l3_3_0801
//! doc: stdlib
//! section: 3.3
//! title: Source of a span, its Slice and sub-slice is the original string for narrow and wide strings
//! rule: new ReadOnlyStringSpan(Source, this.Start + start, length);
//! cites: 3.3
//! expect: run
//! output: True 2 3 1 5
//!   True 2 3 1 11
//!   True 2 3 1 5
//!   True 2 3 1 5
//! issue: 8135
//! notes: Source keeps the whole string; sub = Slice(1,2) starts at 1+1=2, subsub = Slice(1) is Slice(1, 2-1) so starts at 3 with length 1. Length counts code points.
module Conf;

public class Program
  public static method Get(s: ReadOnlyStringSpan): string
    return s.Source;
  end Get;
  public static method Main()
    for var w in ["hello", "héllo wörld", "你好世界你", "\U0001F600abc\U0001F601"] do
      var sp := new ReadOnlyStringSpan(w, 1, 3);
      var sub := sp.Slice(1, 2);
      var subsub := sub.Slice(1);
      var same := sp.Source = w and sub.Source = w and subsub.Source = w and Get(subsub) = w;
      Console.WriteLine($"{same} {sub.Start} {subsub.Start} {subsub.Length} {sp.Source.Length}");
    end for;
  end Main;
end Program;
