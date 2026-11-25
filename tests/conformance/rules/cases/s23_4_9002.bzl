//! case: s23_4_9002
//! doc: spec
//! section: 23.4
//! title: an override of an abstract indexer with an empty setter is dispatched to, 200 writes
//! rule: Indexers may be overloaded by parameter type list. They participate in
//!   `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 23.3, 12.5
//! expect: run
//! output: get
//!   0
//! notes: Harvested from the #9152 review (q01; the abstract indexer declares both accessors `;`-terminated,
//!   grammar A.4 `Accessor* "end" ";"` with AccessorBody `";"`). Trace: the 200 writes through the
//!   abstract-typed receiver dispatch to the override's empty setter and print nothing; the final read
//!   runs the override's getter, which prints "get" and returns `_v` = 0; then 0 is printed.
module Conf;
public abstract class AB
  public abstract property [i: int]: int get; set; end;
end AB;
public class Impl : AB
  public _v: int := 0;
  public override property [i: int]: int
    get => do Console.WriteLine("get"); return _v; end;
    set => do end;
  end;
  public constructor() end constructor;
end Impl;
public class Program
  public static method Main(): int
    using var a: AB := new Impl();
    for var j in 0 ..< 200 do a[0] := 5; end for;
    Console.WriteLine(a[0]);
    return 0;
  end Main;
end Program;
