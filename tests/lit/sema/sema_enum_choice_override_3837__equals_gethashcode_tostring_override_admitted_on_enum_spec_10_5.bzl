// §10.5 (v0.288, #3837): an enum may spell its replacement of `Equals`,
// `GetHashCode`, and `ToString` with `override` — the two spellings
// (`override` or plain) declare the SAME member; enums dispatch statically
// and have no override chain, so `override` here just names the `Object`
// slot the member fills (no base-signature match is required, unlike a
// class's true virtual-dispatch override).
//
// Interpretive note (CLAUDE.md rule 4): whether `override` legally reaches
// `Equals` taking the enum's OWN type (as opposed to `Equals(Object?)`) is
// explicitly left open by #3837 ("follows the record rule and is not
// decided"). This fix admits `override` there too, matched by name+arity
// like the rest of the trio (the same discipline checkEqualityCoDeclaration
// already uses to recognize `Equals(T)`) — a documented choice, not a spec
// ruling; flag for revisit if #3837's open question is settled the other way.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; enum Priority Low, Medium, High; public override method Equals(other: Priority): bool => int(this) = int(other); public override method GetHashCode(): int => int(this); public override method ToString(): string => "Priority"; end Priority; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
