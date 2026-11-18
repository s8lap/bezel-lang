//! case: s9_7_6434
//! doc: spec
//! section: 9.7
//! title: a record reassigned from the same Split element and returned keeps its
//!   string past the call
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
    var e := new E(f[0], 1);
    e := new E(f[0], 2);
    return e;
  end Get;
  public static method Main()
    var ok := 0;
    for var i in 1..200 do
      var e := Program.Get(i);
      var junk := $"JUNK{i},yy{i}".Split(',');
      if e.Name = $"name{i}" and e.N = 2 then ok := ok + 1; end;
    end;
    Console.WriteLine(ok);
  end Main;
end Program;
