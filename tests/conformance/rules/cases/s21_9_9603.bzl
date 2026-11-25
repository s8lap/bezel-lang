//! case: s21_9_9603
//! doc: spec
//! section: 21.9
//! title: a chain-call statement with a trivial-dispose result draws the unused-result warning
//! rule: An expression statement whose result is a non-`()` type is permitted but
//!   the compiler emits a warning: the result is silently discarded
//! cites: 21.9, 28.9
//! expect: warn
//! diagnostic: discarded
//! issue: 9245
module Conf;

public class W
  public V: int := 3;

  destructor
  end destructor;
end W;

public class T
  public V: int := 3;
end T;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;

  public static method MakeT(): () -> owned T
    return lambda(): T => new T();
  end MakeT;

  public static method MakeV(): () -> ()
    return lambda() => do end;
  end MakeV;

  public static method Run()
    C.MakeT()();
  end Run;
end C;
