//! case: s7_4_0043
//! doc: spec
//! section: 7.4
//! title: passing a narrowed local as a ref argument invalidates its narrowing
//! rule: the variable is passed as a `ref` **or `out`** argument (the callee may — for `out`, must (§22.3) — reassign it. ... (v0.338 §7.4 line 909)
//! cites: 22.3
//! expect: reject
//! diagnostic: nullable
module Conf;

public class Program
  public static method Clear(ref t: string?)
    t := null;
  end Clear;

  public static method Probe(s: string?)
    if s <> null then
      Clear(ref s);
      Console.WriteLine(s.Length);
    end if;
  end Probe;

  public static method Main()
    Probe("ab");
  end Main;
end Program;
