//! case: s9_7_6908
//! doc: spec
//! section: 9.7
//! title: a record local left by continue and break keeps its string for the iterations
//!   that read it
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 9.7, 28.9
//! expect: run
//! stdout: 3876
//! notes: A string a record reads from a collection is a runtime-managed value, so the
//!   record outlives the collection and the source it was read from.
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Get(i: int): int
    var t := 0;
    for var k in 1..6 do
      var f := $"name{i},{k}".Split(',');
      var e := new E(f[0], k);
      if k = 2 then continue; end;
      if k = 5 then break; end;
      t := t + e.Name.Length;
    end;
    return t;
  end Get;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var e := Program.Get(i);
      total := total + e;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
