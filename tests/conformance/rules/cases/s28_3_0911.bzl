//! case: s28_3_0911
//! doc: spec
//! section: 28.3
//! title: an implementation of a generic interface method may return a parameter whose type mentions T
//! rule: together with every plain parameter whose declared type mentions `T` ... inherited by
//!   implementations and overrides
//! cites: 11.2, 14.1
//! expect: run
//! output: 8
module Conf;

public interface IEcho<T>
  method Echo(a: T): T;
end IEcho;

public class Echoer: IEcho<Array<int>>
  public constructor()
  end constructor;

  public method Echo(a: Array<int>): Array<int> => a;
end Echoer;

public class Program
  public static method Main()
    using var e := new Echoer();
    using var a := new Array<int>();
    a.Add(8);
    var r := e.Echo(a);
    Console.WriteLine(r[0]);
  end Main;
end Program;
