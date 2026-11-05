//! case: s7_4_0902
//! doc: spec
//! section: 7.4
//! title: a ReadOnlyStringSpan? parameter narrowed by a <> null test admits Length, Slice, indexing and
//! rule: a null test in either spelling — `x <> null` or `x is not null` (narrowing the true branch)
//! cites: 7.4
//! expect: run
//! output: 3
//!   ll
//!   e
//!   ell
//!   null
//!   2
module Conf;

public class Program
  static method F(p: ReadOnlyStringSpan?): int
    if p <> null then
      Console.WriteLine(p.Length);
      Console.WriteLine(p.Slice(1, 2).ToString());
      Console.WriteLine(p[0]);
      Console.WriteLine(p.ToString());
      return p.Length;
    end if;
    Console.WriteLine("null");
    return -1;
  end F;

  public static method Main()
    var w := "hello";
    var sp := new ReadOnlyStringSpan(w, 1, 3);
    var a := F(sp);
    var b := F(null);
    Console.WriteLine(a + b);
  end Main;
end Program;
