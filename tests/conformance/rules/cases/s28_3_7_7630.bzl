//! case: s28_3_7_7630
//! doc: spec
//! section: 28.3.7
//! title: a lambda body may not move out of a using var
//! rule: it cannot be moved out of, only borrowed from
//! cites: 28.3.7, 28.10
//! expect: reject
//! diagnostic: using var
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
      using var q := new Res(1);
      return Sink.Take(move q);
    end;
    Console.WriteLine(f());
  end Main;
end Program;
