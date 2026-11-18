//! case: s9_7_6433
//! doc: spec
//! section: 9.7
//! title: a record built from a Split element and returned out of a loop keeps its
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
    var f := $"a{i},b{i},c{i},d{i}".Split(',');
    for var k in 0..3 do
      var e := new E(f[k], k);
      if k = 1 then continue; end;
      if k = 3 then break; end;
      if k = 2 then return e; end;
    end;
    return new E("none", -1);
  end Get;
  public static method Main()
    var ok := 0;
    for var i in 1..200 do
      var e := Program.Get(i);
      var junk := $"JUNK{i},yy{i}".Split(',');
      if e.Name = $"c{i}" and e.N = 2 then ok := ok + 1; end;
    end;
    Console.WriteLine(ok);
  end Main;
end Program;
