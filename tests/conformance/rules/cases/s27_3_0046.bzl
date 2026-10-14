//! case: s27_3_0046
//! doc: spec
//! section: 27.3
//! title: a block lambda whose every exit path returns a value is a value lambda at () -> int
//! rule: Inside a value-returning block-bodied lambda, `return` is required on every exit path. (v0.338 §27.3)
//! cites: 27.3
//! expect: run
//! output: 4
module Conf;

public class Program
  public static method Main()
    var k: int := 4;
    var f: () -> int := lambda() => do
      if k = 4 then return k; end if;
      return 5;
    end;
    Console.WriteLine(f().ToString());
  end Main;
end Program;
