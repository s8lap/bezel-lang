//! case: s21_9_9501
//! doc: spec
//! section: 21.9
//! title: a bare chain-call statement whose result is a non-trivial-dispose class is a hard error
//! rule: the warning is replaced by a hard error directing the caller to the discard `_ := expression;`
//! cites: 21.9, 17.13
//! expect: reject
//! diagnostic: discard
//! issue: 9245
//! notes: Found by the h17 review (#9245). The call `C.Make()()` is a statement whose result W has a
//!   destructor (non-trivial dispose), so the unused-result warning is replaced by a hard error. The
//!   neighbouring spellings `var f := C.Make(); f();` and `C.Mk();` are hard errors already.
//!   Accepting twin: s21_9_9502.
module Conf;

public class W
  public V: int := 3;
  destructor
  end destructor;
end W;

public class C
  public static method Make(): () -> owned W
    return lambda(): W => new W();
  end Make;

  public static method Main(): int
    C.Make()();
    return 0;
  end Main;

end C;
