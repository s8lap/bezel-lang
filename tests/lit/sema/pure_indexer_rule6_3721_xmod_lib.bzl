// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for the #3721 cross-module indexer
// accessor-read pair (pure_indexer_rule6_3721_xmod_reject.bzl /
// pure_indexer_rule6_3721_xmod_accept.bzl). `Bag`'s indexer `get` is not
// `pure`-declared and writes a static field, so an importer's `pure` body
// may not read `b[i]` over it; `PureBag`'s indexer IS `pure get`-declared,
// so the character-identical shape over it must admit — under §22.15
// rule 6 / §2.8.2 the artifact interface. Two classes, not one indexer
// overloaded twice: an indexer is overloaded by parameter TYPE LIST
// (§23.4), so a single class cannot declare two `[i: int]` indexers. Its
// own RUN line is an unremarkable library accept (§2.4).

module IdxLib3721;

public class Bag
  public static Calls: int := 0;
  public property [i: int]: int
    get => do Calls := Calls + 1; return i + Calls; end;
  end;
end Bag;

public class PureBag
  private _n: int := 7;
  public property [i: int]: int
    pure get => i + _n;
  end;
end PureBag;
