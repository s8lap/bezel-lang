//! case: s28_10_0075
//! doc: spec
//! section: 28.10
//! title: a move of the source after the pinning using var's scope has ended disposes the object
//!   once
//! rule: the frozen kills (a move, a transfer, a reassignment of the binding) are compile errors
//! cites: 17.8, 28.3
//! expect: run
//! output: 100
module Conf;

public sealed class R
  public static Count: int := 0;
  public constructor() end constructor;
  destructor R.Count := R.Count + 1; end destructor;
  public method Use(): int => 1;
  public static method Take(owned r: R)
    using var k := move r;
  end Take;
end R;

public class Program
  public static method Body()
    var a := new R();
    var b := a;
    do
      using var p := b;
      var u := p.Use();
    end;
    R.Take(move a);
  end Body;

  public static method Main(): int
    for var i in 0..<100 do
      Body();
    end;
    Console.WriteLine($"{R.Count}");
    return 0;
  end Main;
end Program;
