// False-reject control for #4689 (§14.2): the two ADMITTED named-type
// bound forms — a class bound and an interface bound — must keep compiling
// clean. Without this control a fix that rejects every named-type bound
// (rather than only Record/Enum/Choice ones) would pass the reject tests
// above and still be wrong.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;

public class Animal end Animal;

public interface IFoo
  method Bar(): int;
end IFoo;

public class BoxClass<T> where T: Animal
  public V: T?;
end BoxClass;

public class BoxIface<T> where T: IFoo
  public V: T?;
end BoxIface;

public class C
  public static method Main(): int
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
