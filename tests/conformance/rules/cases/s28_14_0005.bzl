//! case: s28_14_0005
//! doc: spec
//! section: 28.14
//! title: a plain function-typed parameter is call-bounded and any closure may be passed to it
//! rule: The caller side is correspondingly unrestricted — any closure,
//!   capture-bearing or not, may be passed to a call-bounded parameter,
//!   since every captured source trivially outlives the call.
//! cites: 28.14
//! expect: run
//! stdout: ping
module Conf;

public class Widget
  public constructor()
  end constructor;

  public method Ping()
    Console.WriteLine("ping");
  end Ping;
end Widget;

public class Runner
  public static method Invoke(cb: () -> ())
    cb();
  end Invoke;
end Runner;

public class Program
  public static method Main()
    var w := new Widget();
    Runner.Invoke(lambda() => w.Ping());
  end Main;
end Program;
