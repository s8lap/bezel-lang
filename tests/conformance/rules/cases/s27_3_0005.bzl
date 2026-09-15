//! case: s27_3_0005
//! doc: spec
//! section: 27.3
//! title: a value-returning lambda does not convert to a procedure function type
//! rule: A target type, when present, is checked against the procedure-lambda
//!   classification — a procedure-bodied lambda does not convert to a
//!   value-returning function type, nor the reverse.
//! cites: 27.3
//! expect: reject
//! diagnostic: procedure
module Conf;

public class Program
  public method Demo()
    var proc: (int) -> () := lambda(x: int): int => x * 2;
  end Demo;
end Program;
