//! case: s28_3_10_8603
//! doc: spec
//! section: 28.3.10
//! title: a plain setter body of a generic property disposes its owned local on every call
//! rule: An owned local is disposed exactly once on every control-flow path.
//! cites: 28.3.10, 23.3
//! expect: run
//! output: DD4
//!   Db
//! notes: Harvested from the #9073 review (probe s6_generic). G<int>: two sets, each disposes the setter's local r once (DD), then the getter prints 4. G<string>: one set (D), then the getter prints b. The output lines are DD4 and Db.
module Conf;
import System;
public class Res
  public n: int := 0;
  destructor
    Console.Write("D");
  end destructor;
end Res;
public class G<T>
  private _v: T;
  public constructor(v: T) _v := v; end constructor;
  public property P: T
    get => _v;
    set => do var r := new Res(); _v := value; end;
  end;
end G;
public static class Program
  public static method Main(args: Array<string>): int
    var g := new G<int>(1);
    g.P := 3; g.P := 4; Console.WriteLine(g.P);
    var s := new G<string>("a");
    s.P := "b"; Console.WriteLine(s.P);
    return 0;
  end;
end Program;
