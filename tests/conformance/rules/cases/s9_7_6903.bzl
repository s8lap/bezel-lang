//! case: s9_7_6903
//! doc: spec
//! section: 9.7
//! title: a record built from an alias of a parameter collection keeps its string when
//!   returned
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 9.7, 28.9
//! expect: run
//! stdout: 21392
//! notes: A string a record reads from a collection is a runtime-managed value, so the
//!   record outlives the collection and the source it was read from.
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Get(i: int, src: Array<string>): E
    var g := src;
    return new E(g[0], i);
  end Get;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var src := $"name{i},{i}".Split(',');
      var e := Program.Get(i, src);
      total := total + e.Name.Length + e.N;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
