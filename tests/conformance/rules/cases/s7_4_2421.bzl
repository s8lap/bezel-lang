//! case: s7_4_2421
//! doc: spec
//! section: 7.4
//! title: A nullable IReadOnlyList right operand of `in` that is not narrowed is
//!   rejected
//! rule: Member access, method calls, and indexing on a nullable value are compile
//!   errors unless the compiler has narrowed it to non-null
//! cites: 7.4, 17.5
//! expect: reject
//! diagnostic: narrowing
//! notes: Single ground: b: IReadOnlyList<int>? is not narrowed at `7 in b` (the
//!   call b.Contains(7), 17.5). Removing the `in` expression leaves a compiling
//!   file. Not red on base by verdict: the saved pre-fix compiler also rejected
//!   it, with the 'not yet implemented' text, so only the diagnostic needle
//!   (reported, never gated) differs.4 diagnostic.
module P;

class C
  public static method Q(b: IReadOnlyList<int>?): bool
    return 7 in b;
  end Q;
  public static method Main(): int
    Console.WriteLine(Q(null));
    return 0;
  end Main;
end C;
