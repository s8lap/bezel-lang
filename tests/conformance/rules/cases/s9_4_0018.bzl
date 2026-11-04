//! case: s9_4_0018
//! doc: spec
//! section: 9.4
//! title: a closure over this in a borrow-free record copies the receiver and may be returned
//! rule: A **closure** formed in the body — a lambda that names `this`,
//!   explicitly or through a bare member read resolving to the receiver, … —
//!   captures the fixed receiver value by copy, taken at the closure's
//!   formation into its heap capture cell (§27.5) … nothing where the type
//!   holds no borrows (§3.6), so such a closure is capture-free and goes
//!   wherever a capture-free function value goes — returned, stored, passed to
//!   a set-erasing position —
//! cites: 27.5, 28.3
//! expect: run
//! stdout: 7
module Conf;

public record Pt(X: int)
  public method Adder(): (int) -> int => lambda(d: int) => this.X + d;
end Pt;

public class Program
  public static method Main()
    var p := new Pt(3);
    var f := p.Adder();
    p := new Pt(100);
    Console.WriteLine(f(4));
  end Main;
end Program;
