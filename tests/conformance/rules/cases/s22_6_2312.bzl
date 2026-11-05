//! case: s22_6_2312
//! doc: spec
//! section: 22.6
//! title: a lambda returning an Array of arrays through SelectMany flattens
//!   in order
//! rule: the candidate is applicable at the position when the lambda's
//!   inferred return type equals the candidate's return slot, implicitly
//!   converts to it
//! cites: 5.1, 5
//! expect: run
//! output: 30
//! notes: Accepting control (green on base and tip).
module Conf;

public class Row
  public Items: Array<int>;
  public constructor(a: Array<int>) Items := a; end constructor;
end Row;

public class Program
  public static method Main()
    var rows: Array<Row> := [new Row([1, 2]), new Row([3, 4, 5])];
    var t := 0;
    for var v in rows.SelectMany(lambda(r) => r.Items) do t := t + v; end;
    for var v in rows.SelectMany(lambda(r) => do return r.Items; end) do t := t + v; end;
    Console.WriteLine(t);
  end Main;
end Program;
