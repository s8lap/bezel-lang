//! case: s22_16_0020
//! doc: spec
//! section: 22.16
//! title: a static clause that is a constant expression folding to true is proven
//! rule: The current prover floor — exactly two things inhabit the proven tier: a clause whose
//!   expression is a constant expression folding to `true` — for a `requires` clause, read at each
//!   **discharge instance**, the clause after a call's substitution … a `static requires` is proven
//!   because every call it admits is an instance folding `true`
//! cites: 22.16, 17.14
//! expect: accept
module Conf;

public class Ops
  public static method AlwaysOk(x: int): bool
    static requires(1 = 1)

    return true;
  end AlwaysOk;
end Ops;
