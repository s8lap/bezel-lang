//! case: s14_2_6648
//! doc: spec
//! section: 14.2
//! title: bound `where S: Y` at a `W<Animal>` receiver: non-conforming candidate rejected
//! rule: A constraint whose bound is another type parameter — of the same declaration, or of the enclosing type — is grammatical and has subtype semantics: a substitution S for `T` satisfies `where T: U` exactly when S is non-nullable and is a *subtype of* the substitution for `U`
//! cites: 14.1, 14.2
//! expect: reject
//! diagnostic: type argument
//! issue: 8663
module Conf;
interface IBox<T>
  method Tag(): int;
end IBox;
class Num: IBox<Num>
  public constructor()
  end constructor;
  public method Tag(): int => 3;
end Num;
class Num2: IBox<Num2>
  public constructor()
  end constructor;
  public method Tag(): int => 4;
end Num2;
class D<Y>
  public constructor()
  end constructor;
  public method Z<S>(a: S): int where S: IBox<Y> => a.Tag();
  public method ZL<S>(a: S): int where S: IBox<D<Y>> => a.Tag();
end D;
class Animal
  public constructor()
  end constructor;
end Animal;
class Dog: Animal
end Dog;
class Rock
  public constructor()
  end constructor;
end Rock;
class W<Y>
  public constructor()
  end constructor;
  public method ZY<S>(a: S): int where S: Y => 6;
end W;
class P
  public static method Main(): int
    var w := new W<Animal>();
    Console.WriteLine(w.ZY<Rock>(new Rock()));
    return 0;
  end Main;
end P;
