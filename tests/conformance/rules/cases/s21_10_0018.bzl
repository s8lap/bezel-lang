//! case: s21_10_0018
//! doc: spec
//! section: 21.10
//! title: a local record supports with-expressions and this with inside its own method
//! rule: "are all computed by their existing rules from the declaration."
//! cites: 9
//! expect: run
//! output: 2
//!   2
//!   9
module Conf;

public class Program
  public static method Main()
    record P(A: int, B: int)
      public method Bump(): P
        return this with(A := A + 1);
      end Bump;
    end P;
    var q := new P(1, 2).Bump();
    Console.WriteLine(q.A);
    Console.WriteLine(q.B);
    var r := q with(B := 9);
    Console.WriteLine(r.B);
  end Main;
end Program;
