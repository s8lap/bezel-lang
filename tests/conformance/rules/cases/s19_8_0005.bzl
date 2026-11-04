//! case: s19_8_0005
//! doc: spec
//! section: 19.8
//! title: a nested positional case head reads the slot's choice, not the scrutinee's same-named case
//! rule: The same omission applies within a `case` over a choice scrutinee
//!   (§35.5): a case name resolves to a case pattern, never a type test. At a
//!   nested slot of a destructuring pattern, enum and choice alike, the omission is
//!   read against the slot's declared type, not the scrutinee's
//! cites: 19.8, 35.5
//! expect: run
//! output: inner 5
//!   inner x
//!   box 7
//! notes: both choices declare a case `W` of different arity; `Wrap(W(var p, var q))`
//!   names `Inner.W` (two slots), the top-level `W(var n)` names `Box.W` (one slot).
//!   A payload-free case at a positional slot keeps its prefix (`Inner.X`).
module Conf;

public choice Inner
  W(A: int, B: int),
  X;
end Inner;

public choice Box
  Wrap(I: Inner),
  W(N: int);
end Box;

public class Program
  public static method Show(b: Box)
    case b of
      Wrap(W(var p, var q)) => Console.WriteLine($"inner {p + q}");,
      Wrap(Inner.X)         => Console.WriteLine("inner x");,
      W(var n)              => Console.WriteLine($"box {n}");
    end case;
  end Show;

  public static method Main()
    Show(Box.Wrap(Inner.W(2, 3)));
    Show(Box.Wrap(Inner.X));
    Show(Box.W(7));
  end Main;
end Program;
