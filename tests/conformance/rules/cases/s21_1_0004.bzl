//! case: s21_1_0004
//! doc: spec
//! section: 21.1
//! title: a runtime const admits a value-typed choice initializer
//! rule: Language spec §21.1's runtime-`const` locals: "A local `const`
//!     declaration admits any initializer expression", and under *Ownership*:
//!     "The initializer of a runtime `const` must not be an **owned production**
//!     ... Admitted: value-typed expressions (records included ...)". §35.4
//!     rule 3: a choice whose only payload is a `string` is value-like — "an
//!     ordinary copyable value type" — so `Msg.Note("a" + "b")` is a value-typed
//!     expression, not an owned production, and the `const` is admitted.
//! cites: 21.1, 35.4
//! expect: accept
module Conf;

public choice Msg
  Note(text: string),
  Empty;
end Msg;

public class Program
  public static method Make(): Msg
    const c := Msg.Note("a" + "b");
    return c;
  end Make;
end Program;
