//! case: s22_2_3603
//! doc: spec
//! section: 22.2
//! title: a procedure's arrow body over an owned destructor-bearing result is the discard hard error
//! rule: On a method with **no declared return type**, an expression body is the one-statement block
//!   `expression;` it replaces, and its result is under §21.9's expression-statement regime exactly as that
//!   statement's would be: a `()` result draws nothing (`method G() => Log("x");`); any other result draws
//!   the discard warning (`method F() => 5;`) … and a non-trivial-dispose class, interface-typed or
//!   `Task`-family result is §21.9's hard error
//! cites: 22.2, 21.9, 28.9
//! expect: reject
//! diagnostic: discard
//! issue: 9246
//! notes: The remedy is a block body with `_ := Program.Make();`.
module Conf;

public class W
  public constructor() end constructor;
  destructor end destructor;
end W;

public class Program
  public static method Make(): owned W => new W();
  public static method Fire() => Program.Make();
end Program;
