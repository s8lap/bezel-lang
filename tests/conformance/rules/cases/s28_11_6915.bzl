//! case: s28_11_6915
//! doc: spec
//! section: 28.11
//! title: a string key read by for-in over Keys survives Clear of its dictionary
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 28.11, 9.7, 28.9
//! expect: run
//! stdout: 892
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      using var dk := new Dictionary<string, int>();
      dk.Add($"k{i}", 1);
      for var k in dk.Keys do rows.Add(new E(k, 1)); end;
      dk.Clear();
      var junk := $"JUNK{i},yy{i}".Split(',');
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
