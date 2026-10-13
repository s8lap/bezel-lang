//! case: s28_3_0153
//! doc: spec
//! section: 28.3
//! title: a fresh value, a transfer and a static-storage read may each be stored into a mutable static
//! rule: the remedies are a fresh value (`_m := new Mutex();`), a transfer from an
//!   owned binding (`_m := move m;` — the owner gives the object up and it
//!   becomes process-lived), or static storage (`_m := Other._shared;`)
//! cites: 28.3, 22.12
//! expect: run
//! stdout: ok
//! issue: 5702
module Conf;


import System.Threading;

public class Other
  public static readonly Shared: Mutex := new Mutex();
end Other;

public class Gate
  private static _m: Mutex? := null;

  public static method Adopt(owned m: Mutex)
    _m := move m;
  end Adopt;

  public static method FromStatic()
    _m := Other.Shared;
  end FromStatic;

  public static method Fresh()
    _m := new Mutex();
  end Fresh;

  public static method Main()
    Adopt(new Mutex());
    FromStatic();
    Fresh();
    Console.WriteLine("ok");
  end Main;
end Gate;
