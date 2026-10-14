//! case: s15_3_0018
//! doc: spec
//! section: 15.3
//! title: a local-to-local copy of a value-like choice is independent of its source
//! rule: **Local-to-local assignment** `var y := x;`: copies for an ordinary value type; moves for a move-only value type.
//! cites: 19.3, 35.4
//! expect: run
//! stdout: 690
module Conf;

choice P Ok(v: string), Two(a: string, b: string), No; end P;

class Program
  static method Step(k: int, q: string): int
    var n := 0;
    var a := P.Ok(q);
    var b := a;
    case b of Ok(var s) => do b := P.No; n := s.Length; a := P.No; end, Two(var x, var y) => do n := 0; end, No => do n := 0; end end case;
    return n;
  end Step;

  public static method Main(): int
    var total := 0;
    for var k in 0..<200 do total := total + Program.Step(k, "q" + k.ToString()); end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end Program;
