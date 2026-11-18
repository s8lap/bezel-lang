//! case: s27_5_7206
//! doc: spec
//! section: 27.5
//! title: assigning to a for-loop variable after a lambda captured it is
//!   visible to that lambda
//! rule: Modifications to a captured variable inside the lambda are visible
//!   outside, and vice versa.
//! cites: 27.5, 21.4
//! expect: run
//! stdout: 12000
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
  public static method Main()
    var t := 0;
    for var r in 0..<200 do
      var g := new G();
      for var i in 0..<4 do
        g.Cb := lambda(x: int) => x + i;
        i := i + 10;
        t := t + g.Call(0);
      end for;
      t := t + g.Call(1);
    end for;
    Console.WriteLine(t);
  end Main;
end Program;
