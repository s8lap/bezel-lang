//! case: s7_4_2422
//! doc: spec
//! section: 7.4
//! title: A local declared nullable with a null initializer is not narrowed at `in`
//! rule: Member access, method calls, and indexing on a nullable value are compile
//!   errors unless the compiler has narrowed it to non-null
//! cites: 7.4, 17.5
//! expect: reject
//! diagnostic: narrowing
//! notes: Single ground: the declaration's initializer is `null`, not a non-nullable
//!   expression, so 7.4's declaration-narrowing form does not apply and c is
//!   still nullable at `7 in c`. The accepting twin is the next case (the same
//!   declaration with a non-null initializer). Removing the `in` expression
//!   leaves a compiling file. Not red on base by verdict: the saved pre-fix
//!   compiler also rejected it, with the 'not yet implemented' text, so only
//!   the diagnostic needle (reported, never gated) differs.
module P;

class C
  public static method Main(): int
    var c: IList<int>? := null;
    Console.WriteLine(7 in c);
    return 0;
  end Main;
end C;
