//! case: s3_2_0002
//! doc: spec
//! section: 3.2
//! title: reference-typed assignment aliases the same heap object
//! rule: A reference type is shared on assignment; multiple variables may
//!   refer to the same heap-allocated object.
//! cites: 3.2
//! expect: run
//! stdout: 1
module Conf;

public class Counter
  public property Value: int;

  public constructor()
    Value := 0;
  end constructor;

  public method Increment() => Value := Value + 1;
end Counter;

public class Program
  public static method Main()
    using var a := new Counter();
    var b := a;
    b.Increment();
    Console.WriteLine($"{a.Value}");
  end Main;
end Program;
