//! case: l6_10_0010
//! doc: stdlib
//! section: 6.10
//! title: a TaskScope borrow passed at a generic parameter is an inferred type argument and is rejected
//! rule: `TaskScope` is admissible as no type argument at all, explicit or
//!   inferred, at any instantiation site ... a borrow passed to a generic
//!   parameter is the type-argument case above.
//! cites: 6.10, 14.1
//! expect: reject
//! diagnostic: type argument
//! issue: 5657
module Conf;

import System.Threading;

public class Program
  public static method Log<T>(x: T)
    Console.WriteLine("logged");
  end Log;

  public static async method Run(): Task
    await using var scope := new TaskScope();
    Log(scope);
    await scope.Join();
  end Run;
end Program;
