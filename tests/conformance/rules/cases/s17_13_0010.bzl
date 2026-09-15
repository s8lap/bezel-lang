//! case: s17_13_0010
//! doc: spec
//! section: 17.13
//! title: a lambda parameter _ is a true ignore — several may coexist
//! rule: "a **lambda parameter** ... the ignored slot still contributes to
//!   the function type, and because nothing is named several ignores may
//!   coexist (`lambda(_, _) => 0` is legal)."
//! cites: 17.13, 27.3
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var f: (int, int) -> int := lambda(_, _) => 0;
    Console.WriteLine(f(5, 9));
  end Main;
end Program;
