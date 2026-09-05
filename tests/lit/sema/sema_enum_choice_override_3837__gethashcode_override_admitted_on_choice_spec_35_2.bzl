// §35.2 (v0.288, #3837): a choice body admits exactly what §10.5 admits an
// enum's — `override` on `Equals`/`GetHashCode`/`ToString`, spelled either
// `override` or plain, declaring the SAME member (§35.2 carries §10.5's
// rule over "for the boxable classifications").
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M; choice Shape Circle(radius: double), Square(side: double); public override method GetHashCode(): int => 0; end Shape; class C public static method Main(): int return 0; end Main; end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
