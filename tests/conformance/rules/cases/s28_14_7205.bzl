//! case: s28_14_7205
//! doc: spec
//! section: 28.14
//! title: a constructor that only invokes its plain function-typed
//!   parameter is accepted
//! rule: Within the callee the callback may be invoked, bound to locals,
//!   and passed on as an argument to another call-bounded parameter
//! cites: 28.14, 27.5
//! expect: run
//! stdout: 1780
module Conf;

public class K
  public v: int;

  public constructor(f: () -> int)
    v := f();
  end constructor;
end K;

public class Program
  public static method Main()
    var i := 0;
    var tot := 0;
    while i < 200 do
      var s := "hi" + i.ToString();
      using var a := new K(lambda(): int => s.Length);
      tot := tot + a.v + s.Length;
      i := i + 1;
    end while;
    Console.WriteLine(tot);
  end Main;
end Program;
