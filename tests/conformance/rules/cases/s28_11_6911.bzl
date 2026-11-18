//! case: s28_11_6911
//! doc: spec
//! section: 28.11
//! title: a record built from a for-in string stays readable after its source array is
//!   cleared
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 28.11, 9.7, 28.9
//! expect: run
//! stdout: 2184
//! notes: Store copies the value in; the string it holds is runtime-managed and outlives
//!   the cleared source array.
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      using var strs := $"fa{i},fb{i}".Split(',');
      for var s in strs do rows.Add(new E(s, 1)); end;
      strs.Clear();
      var junk := $"JUNK{i},yy{i}".Split(',');
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
