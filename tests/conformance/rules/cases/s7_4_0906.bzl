//! case: s7_4_0906
//! doc: spec
//! section: 7.4
//! title: ArraySpan<int>? and ReadOnlyArraySpan<int>? locals narrowed by a null test admit members
//! rule: a null test in either spelling — `x <> null` or `x is not null` (narrowing the true branch)
//! cites: 7.4
//! expect: run
//! output: 3
//!   20
//!   40
//!   1
//!   40
//!   99
//!   2
//!   40
//!   True
//!   nullz
module Conf;

public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(10);
    a.Add(20);
    a.Add(30);
    a.Add(40);
    var s: ArraySpan<int>? := a.AsSpan(1, 3);
    if s <> null then
      Console.WriteLine(s.Length);
      Console.WriteLine(s[0]);
      s[0] := 99;
      Console.WriteLine(s.Slice(1, 2)[1]);
      Console.WriteLine(s.IndexOf(30));
      s.Reverse();
    end if;
    Console.WriteLine(a[1]);
    Console.WriteLine(a[3]);
    var r: ReadOnlyArraySpan<int>? := a.AsReadOnlySpan(0, 2);
    if r <> null then
      Console.WriteLine(r.Length);
      Console.WriteLine(r[1]);
      Console.WriteLine(r.Contains(10));
    end if;
    var z: ArraySpan<int>? := null;
    if z <> null then
      Console.WriteLine("bad");
    else
      Console.WriteLine("nullz");
    end if;
  end Main;
end Program;
