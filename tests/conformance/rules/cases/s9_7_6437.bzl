//! case: s9_7_6437
//! doc: spec
//! section: 9.7
//! title: a returned record from a Split element keeps its string after the caller
//!   stores it in a Dictionary
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
    return new E(f[0], i);
  end Get;
  public static method Main()
    var ok := 0;
    var d := new Dictionary<int, E>();
    for var i in 1..200 do
      d.Add(i, Program.Get(i));
      var junk := $"JUNK{i},yy{i}".Split(',');
      if d[i].Name = $"name{i}" then ok := ok + 1; end;
      d.Clear();
    end;
    Console.WriteLine(ok);
  end Main;
end Program;
