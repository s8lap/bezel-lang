//! case: s7_4_0042
//! doc: spec
//! section: 7.4
//! title: reassigning a narrowed local from a non-nullable expression leaves it narrowed
//! rule: after `x := expr;` where `expr`'s static type is non-nullable — or `expr` is itself narrowed non-null at that point — `x` is narrowed until invalidated (v0.338 §7.4 line 903; the invalidation list at lines 906-910 names only reassignment from a nullable-typed expression)
//! cites: 7.4
//! expect: run
//! output: 3
module Conf;

public class Program
  public static method Probe(s: string?, n: string?, k: int)
    if s <> null then
      if k = 1 then s := "xyz"; end if;
      Console.WriteLine(s.Length);
    end if;
  end Probe;

  public static method Main()
    Probe("ab", null, 1);
  end Main;
end Program;
