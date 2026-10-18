//! case: s35_7_0007
//! doc: spec
//! section: 35.7
//! title: a `T?` payload at a generic choice argument renders `null` when absent
//! rule: The generated ToString produces TypeName.CaseName(field1: value1, ...),
//!   and TypeName.CaseName for a payload-free case — and §8.5's choice row
//!   fixes the head UNQUALIFIED ("Status.Active(code: 7)", not
//!   "Conf.Status.Active(code: 7)"). Each payload renders as its own ToString
//!   renders it. A `T?` payload is an admissible component (§9.2 admits `T?`
//!   where `T` is admissible, and §35.7 adds another auto-hashable choice as an
//!   admissible component — `Col?` is one), so the generated rendering exists
//!   and an absent payload renders as `null`.
//! cites: 35.7, 8.5, 9.2
//! expect: run
//! output: W.Ok(v: null)
//!   W.Ok(v: Col.Rgb(v: 7))
module Conf;

public choice Col
  Red,
  Rgb(v: int);
end Col;

public choice W<T>
  Ok(v: T?),
  No;
end W;

public class Program
  public static method Main()
    var pn: Col? := null;
    var pv: Col? := Col.Rgb(7);
    Console.WriteLine(W<Col>.Ok(pn).ToString());
    Console.WriteLine(W<Col>.Ok(pv).ToString());
  end Main;
end Program;
