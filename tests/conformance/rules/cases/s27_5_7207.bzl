//! case: s27_5_7207
//! doc: spec
//! section: 27.5
//! title: a closure that writes its captured parameter is seen by the
//!   enclosing method and by later calls
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5
//! expect: run
//! stdout: 84200
module Conf;

public class G
  public Cb: ((int) -> int)?;

  public method Call(x: int): int
    var f := Cb;
    if f is not null then
      return f(x);
    end if;
    return -1;
  end Call;
end G;

public class Program
  public static method F(g: G, i: int): int
    g.Cb := lambda(x: int) => do
      i := i + x;
      return i;
    end;
    var a := g.Call(5);
    return a + i;
  end F;

  public static method Main()
    var t := 0;
    var g := new G();
    for var k in 0..<200 do
      t := t + Program.F(g, k) + g.Call(1) + g.Call(1);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
