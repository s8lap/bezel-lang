//! case: s22_3_9915
//! doc: spec
//! section: 22.3
//! title: An out function parameter takes a source's function value
//! rule: output parameter. The caller writes `out`. The variable need not be initialized before the call.
//! cites: 22.3, 28.9
//! expect: run
//! stdout: 400
//! notes: Harvested from the #9249 review (r9_out_borrow_uninit), a control: the `out` spelling must keep
//!   working next to the `ref` one the fix touches. 2 x 200 = 400. Green on the pre-fix compiler as
//!   well.
module Conf;

public class Hold
  public F: () -> int;
  public constructor(n: int) F := lambda() => n; end constructor;
end Hold;

public class Program
  public static method Copy(out x: () -> int, src: Hold) x := src.F; end Copy;

  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      var b := new Hold(2);
      var v: () -> int;
      Copy(out v, b);
      sum := sum + v();
    end for;
    Console.WriteLine(sum);
    return 0;
  end Main;
end Program;
