//! case: s28_2_8818
//! doc: spec
//! section: 28.2
//! title: a lambda variable read after a conditional discard in a loop is rejected
//! rule: at a join of an explicitly-disposed path with a still-owned path the variable is **maybe-consumed** and a later use is a compile error
//! cites: 28.2, 28.3.10
//! expect: reject
//! diagnostic: maybe
//! notes: Pins #9098 (305cc38a9, the loop back edge). `_ := x;` runs on the first iteration only, so at the loop head x is
//!  maybe-consumed and the read `x.tag` is the conditional-kill compile error.
module Conf;
public class Res
  public tag: int;
  public constructor(t: int) this.tag := t; end constructor;
end Res;
public class C
  public static method F(flag: bool): int
    var f := lambda(flag: bool): int => do
      var x := new Res(1);
      var s := 0;
      for var i in [0, 1] do
        s := s + x.tag;
        if i = 0 then
          _ := x;
        end if;
      end for;
      return s;
    end;
    return f(flag);
  end F;
  public static method Main(): int
    Console.WriteLine($"{C.F(true)}");
    return 0;
  end Main;
end C;
