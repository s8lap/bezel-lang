//! case: s26_5_2413
//! doc: spec
//! section: 26.5
//! title: a call of a method named Count declared by the receiver's own user
//!   interface resolves to that method
//! rule: If `x`'s declared type (or any base type) *declares* any instance
//!   member named `Foo`
//! cites: 26.5
//! expect: run
//! stdout: 7
module Conf;

public interface ICounter
  method Count(): int;
end ICounter;

public class K: ICounter
  public method Count(): int => 7;
end K;

public class Program
  public static method Main()
    var k: ICounter := new K();
    Console.WriteLine(k.Count());
  end Main;
end Program;
