// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
//
// Issue #4587 (§15.3 / §28.7, spec v0.306) accept control: a non-owning
// choice must be unaffected. `Outcome` carries only a plain `int` payload —
// §35.4 classifies it ValueLike (ordinary, copyable), not owning — so
// `c := b;` is an ordinary COPY, not a §15.3 move, and the later
// `case b of ...` reads `b`'s own live (unmoved) copy. This guards the fix's
// scope: `noteOwningChoiceWholeMoveRead` gates on
// `common::classifyChoiceType(...) == ChoiceClassification::Owning`, so an
// over-broad implementation that fired on every choice type regardless of
// classification would over-reject this file with the same "already moved
// out" diagnostic the owning-choice reject tests pin.

module M;
choice Outcome
  Ready(n: int),
  Empty;
end Outcome;
class C
  public static method Main(): int
    var b := Outcome.Ready(1);
    var c := Outcome.Ready(2);
    c := b;
    case b of
      Ready(_) => do end,
      Empty => do end
    end;
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
