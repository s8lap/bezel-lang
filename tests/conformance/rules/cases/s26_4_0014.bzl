//! case: s26_4_0014
//! doc: spec
//! section: 26.4
//! title: mutually recursive extension methods keep T bound while swapping, in lambdas too
//! rule: The block-level type parameter is in scope throughout the block. ... Each member of a generic extension block is read ... as a generic method that declares the block's parameters before its own — bound by the receiver at a call or a property read (§26.5 step 2)
//! cites: 26.5, 14.1, 3.6
//! expect: run
//! output: y
//!   n
//!   2.5
//!   q
//!   20000000000
//!   610
module Conf;
extension Array<T>
  public method Ping(n: int): int
    if n = 0 then return 0; end;
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
    return 1 + this.Pong(n - 1);
  end Ping;
  public method Pong(n: int): int
    if n = 0 then return 0; end;
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
    return 1 + this.Ping(n - 1);
  end Pong;
  public method Self(n: int): int
    if n = 0 then return 0; end;
    var t: T := this[0]; this[0] := this[1]; this[1] := t;
    return 1 + this.Self(n - 1);
  end Self;
  public method Lam(): int
    var f := lambda() => do var t: T := this[0]; this[0] := this[1]; this[1] := t; end;
    var i := 0;
    while i < 201 do f(); i := i + 1; end;
    return 1;
  end Lam;
  public method LamArg(): int
    var s := 0;
    var f := lambda(k: int) => do
      var t: T := this[0]; this[0] := this[1]; this[1] := t;
      s := s + k;
    end;
    f(1); f(2); f(3);
    return s;
  end LamArg;
end;
class Program
  public static method Main(): int
    var a: Array<string> := ["x", "y"]; var b: Array<string> := ["m", "n"]; var c: Array<double> := [1.5, 2.5];
    var d: Array<string> := ["p", "q"]; var e: Array<long> := [10000000000L, 20000000000L];
    var r := a.Ping(201) + a.Pong(0) + b.Self(201) + c.Ping(201) + d.Lam() + e.LamArg();
    Console.WriteLine(a[0]); Console.WriteLine(b[0]); Console.WriteLine(c[0]); Console.WriteLine(d[0]); Console.WriteLine(e[0]); Console.WriteLine(r);
    return 0;
  end Main;
end Program;
