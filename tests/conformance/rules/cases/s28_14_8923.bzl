//! case: s28_14_8923
//! doc: spec
//! section: 28.14
//! title: an indexer getter that passes its call-bounded index parameter to `Register` is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively), or forwarded into a `scoped` parameter position
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be forwarded into a `scoped` parameter position
//! notes: Pins #9084. stdlib `CancellationToken.Register(scoped callback: () -> ())` declares its callback
//!   `scoped`, so passing the call-bounded `q` forwards it into a `scoped` position. Twin: 8924.
module Conf;
import System.Threading;

public class Fl
  public constructor() end constructor;
  public property [q: () -> ()]: int
    get => do
      var cts := new CancellationTokenSource();
      var ct := cts.Token;
      var sub := ct.Register(q);
      return 0;
    end;
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
