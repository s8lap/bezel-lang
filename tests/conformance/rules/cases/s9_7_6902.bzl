//! case: s9_7_6902
//! doc: spec
//! section: 9.7
//! title: a record built from a receiver field collection element keeps its string when
//!   returned
//! rule: Reference-typed fields whose declared types are runtime-managed (`string`, `Type`,
//!   function types) do not make the record borrow-bearing because runtime-managed values
//!   have unbounded lifetime by construction
//! cites: 9.7, 28.9
//! expect: run
//! stdout: 2584
//! notes: A string a record reads from a collection is a runtime-managed value, so the
//!   record outlives the collection and the source it was read from.
module Conf;
record E(Name: string, N: int);
public class Reg
  private owned _names: Array<string> := new Array<string>();
  public method Put(s: string) this._names.Add(s); end Put;
  public method Get(i: int): E
    return new E(this._names[i], i);
  end Get;
  public method GetAlias(i: int): E
    var ns := this._names;
    return new E(ns[i], i);
  end GetAlias;
end Reg;
public class Program
  public static method Main()
    var total := 0;
    for var i in 1..200 do
      using var r := new Reg();
      r.Put($"name{i}"); r.Put("b");
      var e := r.Get(0);
      var e2 := r.GetAlias(0);
      total := total + e.Name.Length + e.N + e2.Name.Length;
    end;
    Console.WriteLine(total);
  end Main;
end Program;
