//! case: s25_2_0024
//! doc: spec
//! section: 25.2
//! title: a null literal operand binds the nullable operator formal at its own position, left or right
//! rule: Parameters match operands in order — there is no automatic commutation (4
//!   - v is not matched against operator -(v: V, n: int)).
//! cites: 25.2, 22.6, 5.1
//! expect: run
//! output: 1005
//!   105
//! issue: 6939
module Conf;

public record Pt(X: int, Y: int)
  public static pure method operator +(a: Pt?, b: Pt?): int => (a?.X ?? 100) + (b?.X ?? 1000);
end Pt;

public class Program
  public static method Main()
    var p := new Pt(5, 1);
    Console.WriteLine(p + null);
    Console.WriteLine(null + p);
  end Main;
end Program;
