//! case: s28_3_0003
//! doc: spec
//! section: 28.3
//! title: an unmarked read of an owned binding is always a borrow
//! rule: The no-silent-consume rule: an unmarked read of a class-typed binding never
//!   consumes; `var b := a;` is always a borrow of `a`.
//! cites: C.3
//! expect: run
//! stdout: still owns a
module Conf;

public class Widget
  destructor
    Console.WriteLine("disposed");
  end destructor;
end Widget;

public class Program
  public static method Main()
    var a := new Widget();
    var b := a;
    Console.WriteLine("still owns a");
    a.Dispose();
  end Main;
end Program;
