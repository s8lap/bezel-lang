//! case: s26_5_1809
//! doc: spec
//! section: 26.5
//! title: an IEnumerable<int> extension is not reached on an Atomic<int> receiver
//! rule: Where neither rule 1 nor this search supplies the name, nothing supplies
//!   it: the access names no member and is a compile error at the name, the
//!   diagnostic naming the receiver's static type
//! cites: 4.19
//! expect: reject
//! diagnostic: no member
module Conf;

import System.Threading;

extension IEnumerable<int>
  public method Foo(): int => 7;
end;
class Program
  public static method Main()
    using var x := new Atomic<int>(3);
    Console.WriteLine(x.Foo());
  end Main;
end Program;
