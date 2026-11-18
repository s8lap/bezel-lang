//! case: s28_11_6912
//! doc: spec
//! section: 28.11
//! title: a record stored by indexer-set from a for-in string stays readable after the
//!   source is cleared
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 28.11, 9.7, 28.9
//! expect: run
//! stdout: 1092
//! notes: Indexer-set stores the record value; the string it holds is runtime-managed.
module Conf;
record E(Name: string, N: int);
public class Program
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      var rows := new Array<E>();
      rows.Add(new E("x", 0));
      using var strs := $"fa{i},fb{i}".Split(',');
      for var s in strs do rows[0] := new E(s, 1); end;
      strs.Clear();
      var junk := $"JUNK{i},yy{i}".Split(',');
      for var r in rows do total := total + r.Name.Length + r.N; end;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
