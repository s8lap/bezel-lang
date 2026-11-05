//! case: s7_4_0907
//! doc: spec
//! section: 7.4
//! title: Span<int>? and ReadOnlySpan<int>? over an OwningBuffer narrowed by a null test admit members
//! rule: a null test in either spelling — `x <> null` or `x is not null` (narrowing the true branch)
//! cites: 7.4
//! expect: run
//! output: 3
//!   3
//!   2
//!   5
//!   2
//!   1
//!   0
module Conf;

public class Program
  public static method Main()
    using var b := new OwningBuffer<int>(4);
    b[0] := 1;
    b[1] := 2;
    b[2] := 3;
    b[3] := 4;
    var s: Span<int>? := b.AsSpan(1, 3);
    if s <> null then
      Console.WriteLine(s.Length);
      Console.WriteLine(s[1]);
      s[2] := 77;
      Console.WriteLine(s.Slice(1).Length);
      s.Fill(5);
    end if;
    Console.WriteLine(b[3]);
    var r: ReadOnlySpan<int>? := b.AsReadOnlySpan(0, 2);
    if r <> null then
      Console.WriteLine(r.Length);
      Console.WriteLine(r[0]);
      Console.WriteLine(r.Start);
    end if;
  end Main;
end Program;
