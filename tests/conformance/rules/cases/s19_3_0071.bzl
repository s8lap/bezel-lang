//! case: s19_3_0071
//! doc: spec
//! section: 19.3
//! title: a case payload binding over a value-like choice is a copy, unaffected by the scrutinee
//! rule: For value-type scrutinees, the binding is a copy by the standard value-type rules
//! cites: 15.3, 35.4
//! expect: run
//! stdout: 690
module Conf;

choice P Ok(v: string), Two(a: string, b: string), No; end P;

class Program
  static method Step(k: int, q: string): int
    var n := 0;
    var a := P.Ok(q);
    case a of Ok(var s) => do a := P.No; n := s.Length; end, Two(var x, var y) => do n := 0; end, No => do n := 0; end end case;
    return n;
  end Step;

  public static method Main(): int
    var total := 0;
    for var k in 0..<200 do total := total + Program.Step(k, "q" + k.ToString()); end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end Program;
