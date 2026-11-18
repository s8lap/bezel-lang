//! case: s9_7_6436
//! doc: spec
//! section: 9.7
//! title: a record with two string components from one Split element keeps both past
//!   an instance call
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`,
//!   `Type`, function types) do not make the record borrow-bearing because
//!   runtime-managed values have unbounded lifetime by construction
//! cites: 9.7
//! expect: run
//! stdout: 200
module Conf;
record F(A: string, B: string);
public class Holder
  private _k: int;
  public constructor(k: int) this._k := k; end;
  public method Get(i: int): F
    var f := $"name{i},{this._k}".Split(',');
    return new F(f[0], f[0]);
  end Get;
end Holder;
public class Program
  public static method Main()
    var ok := 0;
    using var h := new Holder(7);
    for var i in 1..200 do
      var e := h.Get(i);
      var junk := $"JUNK{i},yy{i}".Split(',');
      if e.A = $"name{i}" and e.B = $"name{i}" then ok := ok + 1; end;
    end;
    Console.WriteLine(ok);
  end Main;
end Program;
