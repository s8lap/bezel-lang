// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s
// NODIAG-NOT: error:
//
// fix #6458 (codegen, emitChoiceToString, v0.338): a generic choice case
// payload spelled `T?` lost its `?` when the payload type was resolved for
// the §35.7 ToString rendering, so `W<Col>.Ok(pn).ToString()` over a `Col?`
// payload read the nullable-value WRAPPER (`{i8 hasValue; <choiceStruct>}`
// — `nullableValueStructTy`'s `isNullableChoiceTy` arm, #2447) as the BARE
// choice struct. The rendering then indexed payload slot `{2,0}` on a
// two-field wrapper, `CreateExtractValue` returned null, and the compiler
// segfaulted in `llvm::Value::setName` (exit 139, no diagnostic).
//
// The FIRST RUN line is the load-bearing one: pre-fix `bezelc` died on the
// signal and lit reports the non-zero exit as a failure. The second pins
// that the fixed compiler is SILENT on this program — a fix that made the
// choice rendering degrade into a diagnostic would trade a crash for a
// false rejection (a silent-acceptance inversion in the other direction).
//
// This is the THIRD spelling of the defect #6169 fixed at
// `emitChoiceEquality` / `emitChoiceHashCode`; the grep that finds all
// three is `grep -rn 'pTy = \*instTy->genericArgs\[i\]' src/codegen/`.
//
// per §9.2, §8.5, §35.7, v0.338
module M;
choice Col Red, Rgb(v: int); end Col;
choice W<T> Ok(v: T?), No; end W;
class Program
  public static method Main(): int
    var pn: Col? := null;
    var n := W<Col>.Ok(pn);
    Console.WriteLine(n.ToString());
    return 1;
  end Main;
end Program;
