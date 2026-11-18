//! case: s27_3_7201
//! doc: spec
//! section: 27.3
//! title: an expression-bodied unit lambda is admitted at a procedure slot
//! rule: For procedure lambdas, the body may also be a single expression
//!   whose result is discarded
//! cites: 27.3
//! expect: run
//! stdout: x
module Conf;

public class Program
  public static method Main()
    var p: () -> () := lambda() => Console.WriteLine("x");
    p();
  end Main;
end Program;
