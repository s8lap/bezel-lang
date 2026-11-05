//! case: s14_1_1413
//! doc: spec
//! section: 14.1
//! title: a record component of type Guid keeps its type inside Box<Guid>
//! rule: Type parameters of declarations that do not nest share no scope
//! cites: 14.1, 2.7
//! expect: run
//! output: 39
//!   39
module Conf;
public record Pt(X: int, Y: Guid);
public class Box<Guid>
  public method Go(p: Pt): int
    return p.Y.ToString().Length + p.X;
  end Go;
end Box;
public class Program
  public static method Main()
    var p := new Pt(3, Guid.NewGuid());
    var b := new Box<int>();
    Console.WriteLine(b.Go(p));
    var q := new Box<string>();
    Console.WriteLine(q.Go(p));
  end Main;
end Program;
