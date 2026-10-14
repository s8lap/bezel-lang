//! case: s7_4_0044
//! doc: spec
//! section: 7.4
//! title: passing a copy of a narrowed local as a ref argument leaves the original narrowed
//! rule: Local-variable narrowing applies to the copy, which no other thread can observe. (v0.338 §7.4 line 933) The invalidation at line 909 applies to the variable passed as a `ref` or `out` argument: here the argument is a different local, `c`, and `s` is not passed.
//! cites: 7.4, 22.3
//! expect: run
//! output: 2
module Conf;

public class Program
  public static method Clear(ref t: string?)
    t := null;
  end Clear;

  public static method Probe(s: string?)
    if s <> null then
      var c: string? := s;
      Clear(ref c);
      Console.WriteLine(s.Length);
    end if;
  end Probe;

  public static method Main()
    Probe("ab");
  end Main;
end Program;
