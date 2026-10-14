//! case: s7_4_0045
//! doc: spec
//! section: 7.4
//! title: a mutably captured local loses its narrowing at an intervening invocation of a function value
//! rule: the variable is **mutably captured** — assigned, or passed as `ref`/`out`, anywhere inside any lambda of the enclosing function body (§27.5) — and the block performs *any* operation that can transfer control to user code: a method, constructor, accessor, or user-defined-operator call, an invocation of a function value, or an `await` (v0.338 §7.4 line 910)
//! cites: 27.5
//! expect: reject
//! diagnostic: captured
module Conf;

public class Program
  public static method Probe(label: string?)
    var clear := lambda() => do label := null; end;
    var noisy := lambda() => do Console.WriteLine("noisy"); end;
    if label <> null then
      noisy();
      Console.WriteLine(label.Length);
    end if;
    clear();
  end Probe;

  public static method Main()
    Probe("gizmo");
  end Main;
end Program;
