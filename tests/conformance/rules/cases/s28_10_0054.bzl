//! case: s28_10_0054
//! doc: spec
//! section: 28.10
//! title: using var over an owned choice field reached through a named owner does not dispose it
//! rule: When the right-hand side of a `using var` declaration is a borrow rather than an owned value ... the `using var` does not assume a disposal obligation ... at scope end, no `Dispose()` is called, because the value is not owned by this scope. The `using var` still pins the borrow's source for the duration of the scope
//! cites: 28.3
//! expect: run
//! output: 80
module Conf;
public class Res
  public static Count: int := 0;
  public constructor() end constructor;
  destructor Res.Count := Res.Count + 1; end destructor;
end Res;
public choice OC Text(owned r: Res), Empty; end OC;
public record Pt(X: int, Y: int);
public record RS(Name: string, N: int);
public record Wrap(R: Res);
public class Box<T>
  public owned V: T;
  public constructor(owned v: T) V := v; end constructor;
end Box;
public class H
  public static owned readonly SF: Res := new Res();
  public owned C: Res := new Res();
  public owned NC: Res? := new Res();
  public owned F: OC := OC.Text(new Res());
  public owned G: OC? := OC.Text(new Res());
  public Bor: Res? := null;
  public N: int := 7;
  public S: string := "s";
  public P: Pt := new Pt(1, 2);
  public RR: RS := new RS("a", 1);
  public property PN: int => N;
  public property PC: Res => C;
  public method Self(): H => this;
  public method Mk(): owned Res return new Res(); end Mk;
  public method CloneH(): owned H return new H(); end CloneH;
  public constructor() end constructor;
end H;
public class Make
  public static method MkH(): owned H return new H(); end MkH;
  public static method MkHN(): owned H? return new H(); end MkHN;
  public static method MkRes(): owned Res return new Res(); end MkRes;
end Make;
public class Program
  public static method Main(): int
    for var i in 0..<20 do
      do var h := new H(); using var x := h.Self().F; end;
    end;
    Console.WriteLine($"{Res.Count}");
    return 0;
  end Main;
end Program;
