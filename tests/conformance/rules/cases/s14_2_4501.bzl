//! case: s14_2_4501
//! doc: spec
//! section: 14.2
//! title: an implementing method omitting its where clauses inherits the interface
//!   method's INumber bound
//! rule: inherits the overridden or implemented declaration's constraints. Omitting
//!   the `where` clauses is the recommended spelling and means exactly the
//!   inherited set
//! cites: 11.2
//! expect: run
//! output: 11
//! issue: 8259
module Conf;

interface IA
  method S<T>(a: Array<T>): T where T: INumber<T>;
end IA;

abstract class A: IA
  public method S<T>(a: Array<T>): T => a.Sum();
end A;

class D: A
end D;

class Prog
  public static method Main(): int
    var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    var d := new D();
    Console.WriteLine(d.S(a));
    return 0;
  end Main;
end Prog;
