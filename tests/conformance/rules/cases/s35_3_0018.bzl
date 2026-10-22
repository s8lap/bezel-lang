//! case: s35_3_0018
//! doc: spec
//! section: 35.3
//! title: a generic choice instantiated by a generic class is the declared result for bare case constructions
//! rule: "a written choice instantiation inside a generic body (`method M<T>(): Result<T, string>`) fires normally" (§35.3, v0.340): the declared `Opt<T>` of `Holder<T>.Get` supplies the choice, so `No` and `Some(x)` are `Opt<T>.No` and `Opt<T>.Some(x)`.
//! cites: 35.5
//! expect: run
//! output: 7
//!   no
//!   s
//!   s
module Conf;

public choice Opt<T>
  Some(v: T),
  No;
end Opt;

public class Holder<T>
  public x: T;

  public constructor(v: T)
    x := v;
  end constructor;

  public method Get(flag: bool): Opt<T>
    if flag then return No; end;
    return Some(x);
  end Get;

  public method Get2(): Opt<T> => Some(x);
end Holder;

public class Program
  public static method Main()
    var a := new Holder<int>(7);
    var b := new Holder<string>("s");
    Console.WriteLine((case a.Get(false) of Some(var v) => $"{v}", No => "no"));
    Console.WriteLine((case a.Get(true) of Some(var v) => $"{v}", No => "no"));
    Console.WriteLine((case b.Get(false) of Some(var v) => v, No => "no"));
    Console.WriteLine((case b.Get2() of Some(var v) => v, No => "no"));
  end Main;
end Program;
