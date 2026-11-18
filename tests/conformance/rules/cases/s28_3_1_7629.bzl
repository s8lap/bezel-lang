//! case: s28_3_1_7629
//! doc: spec
//! section: 28.3.1
//! title: a lambda body may read an owned local before it is moved
//! rule: the binding is unreadable until reassigned
//! cites: 28.3.7
//! expect: run
//! stdout: 2
module Conf;
import System;
class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
  destructor end destructor;
end Res;
class Sink
  public static method Take(owned r: Res): int return r.tag; end Take;
end Sink;
public class Program
  public static method Main()
    var f := lambda(): int => do
      var x := new Res(1);
      var t := x.tag;
      var k := Sink.Take(move x);
      return t + k;
    end;
    Console.WriteLine(f());
  end Main;
end Program;
