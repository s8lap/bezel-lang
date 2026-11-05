//! case: s7_4_2420
//! doc: spec
//! section: 7.4
//! title: A nullable IList right operand of `in` that is not narrowed is rejected
//! rule: Member access, method calls, and indexing on a nullable value are compile
//!   errors unless the compiler has narrowed it to non-null
//! cites: 7.4, 17.5
//! expect: reject
//! diagnostic: narrowing
//! notes: Single ground: `7 in b` is the call b.Contains(7) (17.5) and b:
//!   IList<int>? is not narrowed. The accepting twin is s7_4_2424 (the same
//!   call under a null test). Replacing `7 in b` by `false` leaves a compiling
//!   file. Not red on base by verdict: the saved pre-fix compiler also rejected
//!   the program, with 'not yet implemented: codegen for this `in` expression
//!   form' instead of the 7.4 diagnostic, so only the diagnostic needle
//!   (reported, never gated) differs.
module P;

class C
  public static method Q(b: IList<int>?): bool
    return 7 in b;
  end Q;
  public static method Main(): int
    var a := new Array<int>();
    a.Add(7);
    Console.WriteLine(Q(a));
    return 0;
  end Main;
end C;
