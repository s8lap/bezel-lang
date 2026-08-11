// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// THE ACCEPT CONTROL for the §31.2.1 arm-type rule — the carve that makes the
// rule's predicate exactly right rather than merely strict.
//
// §31.2.1 (v0.243, proposal 0080, #3046), verbatim: "Every class type named in
// an arm pattern ... must be `Exception` or a class deriving from it (an
// **interface-typed** arm remains admissible — no interface derives from
// `Exception`, but an `Exception`-derived class may implement one, so the arm
// matches exactly the throwables whose class implements the interface)."
//
// So the rule is about CLASS types, and an over-broad predicate that also
// rejected interface-typed arms would be a defect — the deliberate asymmetry
// against §31.1, where an interface-typed OPERAND *is* rejected (see
// sema_throw_operand_type__interface_typed_rejected_31_1.bzl). This file pins
// the accept side in all three named positions: a bare interface arm, a
// binding interface arm, and an interface listed in a multi-type alternative.
//
// Clean-compile ACCEPT control: `bezelc -c` must exit zero and emit no
// diagnostics — in particular none of the §31.2.1 arm-type texts.

module M3046C;
import System.IO;

interface IRetryable end IRetryable;

class TransientError: Exception, IRetryable
  public constructor() base("transient"); end constructor;
end TransientError;

class C
  public static method Bare(): int
    try
      throw new TransientError();
    catch
      IRetryable => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Bare;

  public static method Binding(): int
    try
      throw new TransientError();
    catch
      var r: IRetryable => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Binding;

  public static method Alternative(): int
    try
      throw new TransientError();
    catch
      IOException | IRetryable => do return 1; end,
      var e: Exception => do return 2; end
    end;
    return 0;
  end Alternative;

  public static method Main(): int
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: is not an exception type
// NODIAG-NOT: §
