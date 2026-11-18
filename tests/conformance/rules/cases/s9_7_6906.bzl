//! case: s9_7_6906
//! doc: spec
//! section: 9.7
//! title: a record local built in a loop and never returned does not disturb the returned
//!   record
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 9.7, 28.9
//! expect: run
//! stdout: 800
//! notes: A string a record reads from a collection is a runtime-managed value, so the
//!   record outlives the collection and the source it was read from.
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Get(i: int): E
    for var k in 1..5 do
      var f := $"name{i},{k}".Split(',');
      var e := new E(f[0], k);
      if k = 99 then return e; end;
    end;
    return new E("none", 0);
  end Get;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var e := Program.Get(i);
      total := total + e.Name.Length + e.N;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
