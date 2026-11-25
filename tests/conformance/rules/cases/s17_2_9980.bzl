//! case: s17_2_9980
//! doc: spec
//! section: 17.2
//! title: a skipped `and`/`or` right operand holding an owned chain-call result is never produced
//! rule: `and` and `or` short-circuit: the right operand is not evaluated if the result is determined by the
//!   left.
//! cites: 17.2, 28.3.11
//! expect: run
//! output: 100
//!   0
//!   0
//! notes: Pins the skipped-operand half of #9270 (fix d66898662): the right operands hold an unbound owned
//!   chain-call result and are never evaluated, so W is never constructed or destroyed.
module Conf;
public class W
  public static Made: int := 0;
  public static Drops: int := 0;
  public V: int := 3;
  public constructor()
    W.Made := W.Made + 1;
  end constructor;
  destructor
    W.Drops := W.Drops + 1;
  end destructor;
end W;
public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;
  public static method Run(): int
    var t := 0;
    for var j in 0 ..< 100 do
      var b := j < 0 and C.Make()().V = 3;
      var c := j >= 0 or C.Make()().V = 3;
      if (not b) and c then
        t := t + 1;
      end if;
    end for;
    return t;
  end Run;
  public static method Main(): int
    var t := C.Run();
    Console.WriteLine($"{t}");
    Console.WriteLine($"{W.Made}");
    Console.WriteLine($"{W.Drops}");
    return 0;
  end Main;
end C;
