//! case: s21_9_9604
//! doc: spec
//! section: 21.9
//! title: a chain-call result disposed explicitly in the statement is admitted
//! rule: an explicit `.Dispose()`, or `Task.FireAndForget`
//! cites: 21.9, 28.6
//! expect: accept
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
    C.Make()().Dispose();
  end Run;
end C;
