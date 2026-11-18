//! case: s9_7_6901
//! doc: spec
//! section: 9.7
//! title: a record built from a parameter collection element keeps its string when returned
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
  public static method Wrap(f: Array<string>, i: int): E
    return new E(f[0], i);
  end Wrap;
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var f := $"name{i},{i}".Split(',');
      var e := Program.Wrap(f, i);
      total := total + e.Name.Length + e.N;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
