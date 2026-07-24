// RUN: %bezelc -c -o %t.o %s
//
// §35.12 (v0.226, issue #2211): payload sendability is transitive. A finite
// re-entry into the same generic choice declaration is not a recursive type
// cycle: `Opt<Opt<int>>` reaches the sendable `int` leaf and is sendable. This
// guards the env-threaded walk against a declaration-keyed recursion shortcut.

module M;
choice Opt<T> Some(v: T), None; end Opt;
static class Box
  public static method S<T>(): int where T: sendable => 1;
end Box;
class C
  public static method Main(): int
    return Box.S<Opt<Opt<int>>>();
  end Main;
end C;
