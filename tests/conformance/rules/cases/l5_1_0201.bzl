//! case: l5_1_0201
//! doc: stdlib
//! section: 5.1
//! title: Min without a selector over a record that is not comparable is rejected
//! rule: `Min` and `Max` without a selector are generic over comparable types:
//! cites: 5, 14.2
//! expect: reject
//! diagnostic: IComparable
//! issue: 8092
module Conf;

public record Pt(X: int, Y: int);

public class Program
  public static method Main()
    var a: Array<Pt> := [new Pt(3, 30), new Pt(1, 10)];
    var m := a.Min();
    Console.WriteLine(m.X);
  end Main;
end Program;
