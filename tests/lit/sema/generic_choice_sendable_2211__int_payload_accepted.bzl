// RUN: %bezelc -c -o %t.o %s
//
// §35.11 / §35.12 (v0.226, issue #2211): choice classification and
// sendability are evaluated per instantiation. `Opt<int>` is value-like and
// its bare `T` payload substitutes to sendable `int`, so it satisfies
// `where T: sendable`. Before #2211 the payload walk re-read the declaration
// template's bare `T` without the #2183/#2145 substitution environment and
// over-rejected this exact program.

module M;
choice Opt<T> Some(v: T), None; end Opt;
static class Box
  public static method S<T>(): int where T: sendable => 1;
end Box;
class C
  public static method Main(): int
    return Box.S<Opt<int>>();
  end Main;
end C;
