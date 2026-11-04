//! case: l3_3_0802
//! doc: stdlib
//! section: 3.3
//! title: Source of an empty span, a span held in a record, and string.AsSpan() slices is the original string
//! rule: public record ReadOnlyStringSpan(Source: string, Start: int, Length: int)
//! cites: 3.3, 9.6
//! expect: run
//! output: True 5
//!   True 11
//!   True 5
//!   True 0
//! issue: 8135
module Conf;

public record Holder(S: ReadOnlyStringSpan);

public class Program
  public static method Main()
    for var w in ["hello", "héllo wörld", "你好世界你"] do
      var sp := new ReadOnlyStringSpan(w, 1, 3);
      var e := new ReadOnlyStringSpan(w, 2, 0);
      var h := new Holder(sp);
      var ok := e.Source = w and e.IsEmpty and h.S.Source = w and w.AsSpan().Source = w and w.AsSpan().Slice(2).Source = w;
      Console.WriteLine($"{ok} {h.S.Source.Length}");
    end for;
    var z := new ReadOnlyStringSpan("", 0, 0);
    var zs := z.Source = "";
    Console.WriteLine($"{zs} {z.Source.Length}");
  end Main;
end Program;
