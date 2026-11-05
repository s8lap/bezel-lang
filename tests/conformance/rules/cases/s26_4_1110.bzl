//! case: s26_4_1110
//! doc: spec
//! section: 26.4
//! title: Channel<U> at a method's own parameter U is rejected inside a class whose T is sendable
//! rule: Nothing is inferred and nothing propagates
//! cites: 14.2
//! expect: reject
//! diagnostic: sendable
//! issue: 5998
module Conf;
import System.Threading;

public class Box<T> where T: sendable
  public method F<U>(c: Channel<U>): int
    return 0;
  end F;
end Box;

public class Program
  public static method Main()
  end Main;
end Program;
