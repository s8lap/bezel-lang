//! case: s11_1_0006
//! doc: spec
//! section: 11.1
//! title: Exception subclasses keep their message through primary and explicit constructors
//! rule: "This desugars to a primary constructor that chains to `base(name, balance)` before executing field
//!   initializers" ([§11.1]), so the field `Seen: string := Message` of `E(m: string): Exception(m)`
//!   reads the message that the base constructor set. The `Exception` surface is "public pure
//!   constructor(message: string);" and "public readonly property Message: string;" ([§31.4]). Each of
//!   the six checks holds on all 200 iterations: the own message and Show, a rethrown message, a B with
//!   its Tag, a D through C (whose constructor chains to A with a `!` appended), and W's `/w` message. So
//!   bad stays 0, and the program prints bad=0 and returns 0.
//! cites: 11.1, 31.4
//! expect: run
//! stdout: bad=0
//! issue: 6704
module Conf;

public class E(m: string): Exception(m)
  public Copy: string := m;
  public Len: int := m.Length;
  public Seen: string := Message;
  public method Show(): string
    return $"{Copy}|{Len}|{Message}|{Seen}";
  end Show;
end E;

public class A(m: string): Exception(m)
end A;
public class B(m: string): A(m)
  public Tag: string := m + "#";
end B;

public class C: A
  public constructor(x: string)
    base(x + "!");
  end constructor;
end C;
public class D(m: string): C(m)
end D;

public class W(m: string, c: Exception): Exception(m + "/w", c)
  public Inner: string := c.Message;
end W;

public class Program
  public static method Main(): int
    var bad: int := 0;
    var ok: bool := false;
    var i: int := 0;
    while i < 200 do
      i := i + 1;
      var s: string := i.ToString();
      ok := false;
      try
        throw new E($"msg{s}");
      catch var e: E =>
        ok := not (e.Message <> $"msg{s}" or e.Show() <> $"msg{s}|{s.Length+3}|msg{s}|msg{s}");
      end;
      if not ok then bad := bad + 1; end if;
      ok := false;
      try
        throw new E("x" + s);
      catch var e: Exception =>
        ok := not (e.Message <> "x" + s);
      end;
      if not ok then bad := bad + 1; end if;
      ok := false;
      try
        ok := false;
      try
          throw new E("re" + s);
        catch var e: E =>
          throw;
        end;
      catch var e: Exception =>
        ok := not (e.Message <> "re" + s);
      end;
      if not ok then bad := bad + 1; end if;
      ok := false;
      try
        throw new B(s + "b");
      catch var e: B =>
        ok := not (e.Message <> s + "b" or e.Tag <> s + "b#");
      end;
      if not ok then bad := bad + 1; end if;
      ok := false;
      try
        throw new D(s + "d");
      catch var e: A =>
        ok := not (e.Message <> s + "d!");
      end;
      if not ok then bad := bad + 1; end if;
      ok := false;
      try
        using var inner: E := new E("in" + s);
        throw new W("out" + s, inner);
      catch var w: Exception =>
        ok := not (w.Message <> "out" + s + "/w");
      end;
      if not ok then bad := bad + 1; end if;
    end while;
    Console.WriteLine($"bad={bad}");
    return bad;
  end Main;
end Program;
