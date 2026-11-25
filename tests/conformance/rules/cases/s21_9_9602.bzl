//! case: s21_9_9602
//! doc: spec
//! section: 21.9
//! title: a chain call returning the unit value as a statement draws no diagnostic
//! rule: Method calls returning `()` and `await` expressions on `Task` (no result)
//!   are the common cases; no warning is emitted for these.
//! cites: 21.9
//! expect: quiet
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
    C.MakeV()();
  end Run;
end C;
