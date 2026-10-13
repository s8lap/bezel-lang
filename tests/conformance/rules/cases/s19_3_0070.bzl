//! case: s19_3_0070
//! doc: spec
//! section: 19.3
//! title: a public field named through a generic alias head over a typed scrutinee is accepted
//! rule: Resolution honours ordinary member visibility (13). A public field of a generic record is named by the slot identifier through an alias that instantiates it, Box<int>, and the scrutinee is typed BI, so its static type fixes the instantiation (an Object scrutinee would be refused as a generic record pattern that names no type arguments).
//! cites: 19.3, 13.1
//! expect: run
//! stdout: 7
//! issue: 6045
module Conf;

public record Box<T>
  public h: T;

  public constructor(x: T)
    h := x;
  end constructor;
end Box;

type BI := Box<int>;

public class Program
  public static method Main()
    var r: BI := new Box<int>(7);
    case r of BI(h = var v) => Console.WriteLine(v.ToString());,
      _ => Console.WriteLine("n");
    end case;
  end Main;
end Program;
