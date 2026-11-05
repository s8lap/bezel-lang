//! case: s8_5_0903
//! doc: spec
//! section: 8.5
//! title: a choice's $"{this}" in an async lambda of its own method renders the case form, head unqualified
//! rule: `"TypeName.CaseName(field1: value1, …)"`, and `"TypeName.CaseName"` for a payload-free
//!   case — with the head **unqualified**
//! cites: 35.7, 27.5
//! expect: run
//! output: o O.Some(v: 3)
//! issue: 8127
module Conf;

public choice O
  Some(v: int),
  None;

  public method Show(): string
    var f := async lambda(): Task<string> => $"o {this}";
    using var t := f();
    return t.Result;
  end Show;
end O;

public record R(K: int)
  public method Go(o: O): string
    return o.Show();
  end Go;
end R;

public class Program
  public static method Main()
    var o := O.Some(3);
    var r := new R(10);
    Console.WriteLine(r.Go(o));
  end Main;
end Program;
