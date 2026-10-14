//! case: s27_3_0053
//! doc: spec
//! section: 27.3
//! title: a block lambda whose only return is a lambda takes that lambda's function type as its return type
//! rule: A value-returning block-bodied lambda's return type is inferred only
//!   when every `return e;` expression in the body has the same type. (v0.338
//!   §27.3, Return-type inference without a target)
//! cites: 27.3
//! expect: run
//! output: 7
module Conf;

public class Program
  public static method Main()
    var mk := lambda() => do
      return lambda() => 7;
    end;
    Console.WriteLine(mk()());
  end Main;
end Program;
