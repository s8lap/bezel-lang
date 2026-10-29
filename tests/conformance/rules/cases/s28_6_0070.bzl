//! case: s28_6_0070
//! doc: spec
//! section: 28.6
//! title: a this-returning extension method chains on a fresh StringBuilder temporary 100 times
//! rule: The chain is usable within its full expression only: as an expression statement it runs and the root is disposed at the statement's end ... A method that returns `this` returns a borrow of the receiver
//! cites: 28.6
//! expect: run
//! stdout: ab
module Conf;

extension StringBuilder
  public method Self(): StringBuilder => this;
end;

public class Program
  public static method Main()
    for var i in 0..<100 do
      var s := new StringBuilder("a").Self().Append("b").Self().ToString();
      if i = 99 then
        Console.WriteLine(s);
      end if;
    end for;
  end Main;
end Program;
