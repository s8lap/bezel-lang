//! case: l4_4_0047
//! doc: stdlib
//! section: 4.4
//! title: Array<int>.Sorted(a) static form is rejected: Sorted is an instance member
//! rule: Array<T> declares `Sorted()` and `Sorted(comparer)` as instance methods returning a fresh owned Array<T> and declares no static members; an undeclared static is an error (stdlib 4.4; spec 22.1).
//! cites: 4.4, 22.1
//! expect: reject
//! diagnostic: static
module Conf;
class C
  public static method Main(): int
    var a: Array<int> := [3, 1, 2];
    var b := Array<int>.Sorted(a);
    return a[0];
  end Main;
end C;
