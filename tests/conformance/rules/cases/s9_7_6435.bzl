//! case: s9_7_6435
//! doc: spec
//! section: 9.7
//! title: a record from a Split element, also stored in a local array, keeps its
//!   string when returned
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`,
//!   `Type`, function types) do not make the record borrow-bearing because
//!   runtime-managed values have unbounded lifetime by construction
//! cites: 9.7
//! expect: run
//! stdout: 200
module Conf;
record E(Name: string, N: int);
public class Program
  static method Get(i: int): E
    var f := $"name{i},{i}".Split(',');
    var rows := new Array<E>();
    var e := new E(f[0], i);
    rows.Add(e);
    if i mod 2 = 0 then return e; end;
    return rows[0];
  end Get;
  public static method Main()
    var ok := 0;
    for var i in 1..200 do
      var e := Program.Get(i);
      var junk := $"JUNK{i},yy{i}".Split(',');
      if e.Name = $"name{i}" then ok := ok + 1; end;
    end;
    Console.WriteLine(ok);
  end Main;
end Program;
