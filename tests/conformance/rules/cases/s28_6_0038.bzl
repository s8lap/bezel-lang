//! case: s28_6_0038
//! doc: spec
//! section: 28.6
//! title: the same chain used within its full expression runs and disposes the root at statement end
//! rule: The chain is usable within its full expression only: as an expression
//!   statement it runs and the root is disposed at the statement's end
//! cites: 28.6, 28.3
//! expect: run
//! output: using inner
//!   inner disposed
//!   after
module Conf;

public class Inner
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("inner disposed");
  end destructor;
end Inner;

public class Pool
  private owned _first: Inner := new Inner();

  public method First(): Inner => _first;
end Pool;

public class Demo
  public static method Use(i: Inner)
    Console.WriteLine("using inner");
  end Use;

  public static method Main()
    Use(new Pool().First());
    Console.WriteLine("after");
  end Main;
end Demo;
