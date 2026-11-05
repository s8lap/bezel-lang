//! case: l2_1_0901
//! doc: stdlib
//! section: 2.1
//! title: Console.WriteLine of a class and of an Object binding invokes the overriding ToString
//! rule: The `Object` overloads invoke `ToString()`.
//! cites: 2.1
//! expect: run
//! output: caught=330
module Conf;

public class T
  public constructor()
  end constructor;

  public override method ToString(): string
    throw new InvalidOperationException("render");
  end ToString;
end T;

public class Program
  public static method Main()
    var caught := 0;
    var i := 0;
    while i < 30 do
      var t := new T();
      try
        Console.WriteLine(t);
      catch
        InvalidOperationException => do caught := caught + 1; end,
      end try;
      try
        var o: Object := t;
        Console.WriteLine(o);
      catch
        InvalidOperationException => do caught := caught + 10; end,
      end try;
      i := i + 1;
    end while;
    Console.WriteLine("caught=" + caught.ToString());
  end Main;
end Program;
