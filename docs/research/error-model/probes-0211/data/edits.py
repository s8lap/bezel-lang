#!/usr/bin/env python3
"""The edit set of proposal 0211, machine-checkable.

Each edit names a document, an `old` text that must occur exactly once in it, and the
`new` text. `insert` edits keep `old` and add `add` after it (same paragraph when `add`
starts with a space, a new paragraph when it starts with a blank line).

  edits.py check <repo-root>            every `old` occurs exactly once; the edits apply in order
  edits.py apply <repo-root> <out-dir>  write the patched documents under <out-dir>/docs
  edits.py render                       print the edits as the proposal's section 10 prints them
  edits.py twin <proposal.md>           every `new`/`add` text appears verbatim in the proposal

The proposal's section 10 is the authority; this file is its twin, kept so that the
anchors can be re-verified against HEAD when the proposal graduates.
"""
import os, sys

SPEC = "docs/Bezel_Language_Spec.md"
LIB = "docs/Bezel_Standard_Library.md"
RAT = "docs/Bezel_Design_Rationale.md"
G2 = "docs/Bezel_Guide_02_Errors_and_Nullability.md"
G4 = "docs/Bezel_Guide_04_Concurrency.md"
G6 = "docs/Bezel_Guide_06_Contracts_and_Purity.md"
TOUR = "docs/Bezel_Tour.md"
CG = "docs/Bezel_For_Code_Generation.md"

E = []
def rep(i, doc, where, old, new): E.append(dict(id=i, doc=doc, where=where, kind="replace", old=old, new=new))
def ins(i, doc, where, old, add): E.append(dict(id=i, doc=doc, where=where, kind="insert", old=old, add=add))

# ------------------------------------------------------------------ spec
rep("S1", SPEC, "§2.3, the table's *Standard exception types* row",
    "| Standard exception types | the [§31.4](#sec-standard-exception-types) built-in hierarchy (`Exception` and the types in its tree), whose module is `System`",
    "| Standard exception types | the [§31.4](#sec-standard-exception-types) built-in hierarchy (`Exception` and the types in its tree) and the failure marker `IFailure` ([§31.4](#sec-the-failure-marker)), whose module is `System`")

ins("S2", SPEC, "§31.1 *The throw operand*, end of the last paragraph",
    "Every throwable carries the `Exception` reporting surface — `Message`, `StackTrace`, `CauseChain`, `SupersededChain` ([§31.4](#sec-standard-exception-types)) — and participates in the unhandled-exception report.",
    " A throwable whose class implements `IFailure` is a **failure**; every other is a **defect** report, cancellation apart ([§31.4](#sec-the-failure-marker)). Dispatch reads the kind only where an arm names the marker.")

ins("S3", SPEC, "§31.2.1, the binding-pattern bullet",
    "- A binding pattern: `var name: Type`. Matches and binds the exception to `name` for use in the arm's guard and body. The binding is in scope only within the arm.",
    " `name`'s static type is `Type` where `Type` is a class or a type parameter, and `Exception` where `Type` is an interface (*Interface arm types*, below).")

rep("S4", SPEC, "§31.2.1, the multi-type bullet",
    "When binding, `name` has the static type of the **nearest common base** of the listed types — the most specific class that is a base of every listed type. Every listed type is `Exception`-derived (the arm-type rule below), so the nearest common base is always `Exception` or a class deriving from it; for alternatives sharing nothing more specific, it is `Exception`.",
    "When binding, `name` has the static type of the **nearest common base** of the listed types — the most specific class that is a base of every listed type, an interface counting as `Exception` (*Interface arm types*, below). Every listed class type is `Exception`-derived (the arm-type rule below), so the nearest common base is always `Exception` or a class deriving from it; for alternatives sharing nothing more specific, and for every alternative that lists an interface, it is `Exception`.")

ins("S5", SPEC, "§31.2.1, a new paragraph after *Arm types are `Exception`-family*",
    "the remedy is to delete the arm or fix the type.",
    "\n\n**Interface arm types.** An interface named in an arm is a filter: the arm takes exactly the throwables whose class implements it. It does not type a binding. Every throwable is an instance of an `Exception`-derived class ([§31.1](#sec-the-throw-operand)), and nothing narrower is known of one matched through an interface, so a binding arm over an interface binds `name` at `Exception`, and an interface listed in a binding alternative counts as `Exception` in the nearest-common-base computation: `var f: IFailure` and `var ex: IOException | IRetryable` each bind an `Exception`. The binding therefore reads `Message`, is an argument where an `Exception` is expected, and chains as a cause ([§31.4](#sec-standard-exception-types)), whatever interface selected it. The interface's own members are reached by a type test inside the arm: `var r := ex as IRetryable;` gives a nullable borrow at the interface type and leaves `ex` an `Exception`. The guard form `var ex: Exception when ex is IRetryable` instead retypes the binding for the arm ([§12.7](#sec-type-tests-is-is-not-as)), which then sees the interface's members and no longer the `Exception` surface. This is a difference from `case`, where a type pattern's binding has the pattern's written type ([§19.3](#sec-patterns)): a `case` scrutinee may be of any type, while a caught value is always an `Exception`, and the arm's binding says so. A type parameter used as an arm type keeps its class bound (below): `where TEx: Exception, IFailure`, never the interface alone. The failure marker `IFailure` ([§31.4](#sec-the-failure-marker)) is the interface this rule is written for.")

rep("S6", SPEC, "§31.2.4, the last example's second arm",
    "  var ex: Exception => Log(ex)\nend;",
    "  var f: IFailure => Log(f)                 // every other failure; `f` is an `Exception` (§31.2.1); defects pass\nend;")

ins("S7", SPEC, "§31.2.5, end of the last paragraph",
    "Silent suppression of exceptions is a code smell, and the syntax does not hide it.",
    " To suppress expected failures only, name the marker — `IFailure => ()` — and defects and cancellation still propagate ([§31.4](#sec-the-failure-marker)).")

rep("S8", SPEC, "§31.4, the first paragraph",
    "(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms) are **`pure`**",
    "(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms, and the `()` and `(message)` forms of `ParseRangeException`, stdlib [§9.1](Bezel_Standard_Library.md#sec-integer-types)) are **`pure`**")

rep("S9", SPEC, "§31.4, the hierarchy tree",
    "├─ FormatException                  — string does not conform to an expected format.",
    "├─ FormatException                  — string does not conform to an expected format. A failure: implements `IFailure`.")

ins("S10", SPEC, "§31.4, new paragraphs after the tree's arm-ordering paragraph",
    "and an arm pattern of `ArgumentException` matches `ArgumentNullException`.",
    """

<a id="sec-the-failure-marker"></a>
**The failure marker.** An exception is of one of two kinds, and the kind is a property of its class. A **failure** reports a condition a correct program can meet: the environment, input from outside the program, a peer, or a time or capacity limit produced it. Its class implements the built-in interface `IFailure`. Every other exception is a **defect** report: the program is wrong, and the exception says where. One class is named apart: `OperationCanceledException` reports **cancellation**, the answer to a request the program itself made; it does not implement `IFailure`, and a handler of failures never takes it.

```bezel
public interface IFailure
end IFailure;
```

`IFailure` declares no members: it classifies, and the object it classifies is always reachable as an `Exception` ([§31.2.1](#sec-arm-patterns), *Interface arm types*). It is available without import, with the hierarchy above. A class implements it by listing it, and a class deriving from one that implements it implements it too ([§14.2](#sec-interface-conformance) rule 1), so a kind is inherited and cannot be shed.

**Who may conform.** Only an `Exception`-derived class: a record, an enum, a choice, or a class outside the `Exception` family that lists `IFailure`, or an interface extending it, is a compile error at the declaration. No value of such a type can be thrown ([§31.1](#sec-the-throw-operand)), so the conformance could never be observed — [§31.2.1](#sec-arm-patterns)'s ground for rejecting a dead arm type. And a class does not contradict the kind of a class the documents declare: a class that does not inherit `IFailure` may list it only where no class in its base chain, other than `Exception` itself, is a built-in or standard-library exception class; listing it otherwise is a compile error at the declaration. So a class deriving from `OperationCanceledException` is cancellation in every subclass, and an arm naming `InvalidOperationException`, or any other defect class of the documents, never takes a failure that user code declared. A class whose base chain is user-declared down to `Exception` chooses freely at each level. The standard library declares the only two classes that derive across that line, each by recorded ruling (stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule)). An interface may extend `IFailure`; both restrictions then bind its implementers.

Of the built-in types, `FormatException` implements `IFailure`. The standard library's failure types, and the rule by which a condition is raised as a failure or as a defect, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). A class that this specification or the standard library declares changes kind only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker declares a defect report.

The kind is read in two places. A catch arm that names the marker selects on it: an `IFailure` arm takes every failure and lets defects and cancellation pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)). And a domain join carries it across the boundary ([§30.14](#sec-multiple-domains)). Nothing else reads it. The ownership of a thrown exception ([§31.1](#sec-exception-ownership)), the cleanup an exception edge runs ([K3.15](#sec-kernel-k3)), a task's fault ([§30.9](#sec-exception-handling-in-async-methods)), the purity carve-out ([§22.15](#sec-pure-throw)), supersession ([§31.2](#sec-try-catch-finally)), and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for both kinds and for cancellation.""")

rep("S11", SPEC, "§31.4, the last paragraph",
    "`FileNotFoundException`, `IOException`, and others. Outside the auto-imported modules",
    "`ParseRangeException` (deriving from `OverflowException` and implementing `IFailure`; raised by the `Parse` family for a value outside the type's range; declared in the auto-imported `System`, stdlib §9.1), `FileNotFoundException`, `IOException`, and others. Outside the auto-imported modules")

rep("S12", SPEC, "§30.14 *Faults*",
    "whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). Programs that need richer fault data",
    "whose `Message` and `FaultStackTrace` carry copies of the original exception's message and stack-trace text (`string`s — sendable). The exception's kind crosses with them: where the original exception's class implements `IFailure` ([§31.4](#sec-the-failure-marker)), the join raises `DomainFailedException`, which derives from `DomainFaultedException` and implements `IFailure` (stdlib [§6.8](Bezel_Standard_Library.md#sec-domain)). An arm naming `DomainFaultedException` takes both; an arm naming `IFailure` takes a failure raised in another domain as it takes one raised in its own. Programs that need richer fault data")

rep("S13", SPEC, "§30.14 *Data-parallel combinators*, *Results and faults*",
    "the returned task faults with `DomainFaultedException` — this section's fault vocabulary, with the no-aggregation",
    "the returned task faults with `DomainFaultedException` (`DomainFailedException` where that first fault is a failure) — this section's fault vocabulary, with the no-aggregation")

rep("S14", SPEC, "Appendix C.7, the multi-type binding bullet",
    "- A multi-type catch alternative in binding form types the binding as the listed types' **nearest common base** — with every listed type `Exception`-derived, always `Exception` or a class deriving from it ([§31.2.1](#sec-arm-patterns)).",
    "- A multi-type catch alternative in binding form types the binding as the listed types' **nearest common base** — with every listed class type `Exception`-derived and an interface counting as `Exception`, always `Exception` or a class deriving from it; a binding arm over one interface type types the binding as `Exception` ([§31.2.1](#sec-arm-patterns)).\n- `IFailure` is listed only by a class deriving from `Exception`, or extended by an interface; a record, enum, choice, or class outside the `Exception` family that conforms to it is a compile error at the declaration, and so is a class that does not inherit it and has a built-in or standard-library exception class other than `Exception` in its base chain — `OperationCanceledException` and every defect class among them ([§31.4](#sec-the-failure-marker)).")

ins("S15", SPEC, "Appendix E, a new row after the *public type removed* row",
    "| public type removed; base class changed; implemented interface removed | **breaking** | An interface *added* to a type: widening. |",
    "\n| `IFailure` added to the interfaces of an exception class | **breaking** | The one exception to *an interface added to a type: widening*. The kind is what a consumer's boundary arm selects on ([§31.4](#sec-the-failure-marker)): adding the marker makes every `IFailure` arm take the class. (Removing it is breaking by the row above.) |")

# ------------------------------------------------------------------ standard library
rep("L1", LIB, "§1, the failure-signal rule, band 1",
    "(a bad argument, protocol misuse, a state violation) → a `requires` clause where a contract is possible (language spec [§22.16](Bezel_Language_Spec.md#sec-contract-clauses)); the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on.",
    "(a bad argument, protocol misuse, a state violation, an arithmetic result outside its domain) → a `requires` clause where a contract is possible (language spec [§22.16](Bezel_Language_Spec.md#sec-contract-clauses)); the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries, and the arithmetic classes of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types) (`OverflowException`, `DivideByZeroException`) where a member surfaces an arithmetic fault. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on, and its class does not implement `IFailure` (*Failures and defects*, below).")

rep("L2", LIB, "§1, the failure-signal rule, band 4",
    "→ a typed exception, producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**",
    "→ a typed exception whose class implements `IFailure` (a **failure**; *Failures and defects*, below), producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**")

ins("L3", LIB, "§1, a new paragraph after the band-3/band-4 paragraph",
    "and the no-dual rule below applies to whatever spelling that design fixes.",
    "\n\n**Failures and defects.** The exceptions this library raises are of the kinds language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) defines, and the band fixes the kind: a band-4 condition is raised with a class that implements `IFailure`, a band-1 condition with one that does not, and cancellation with `OperationCanceledException`. The test between bands 1 and 4 is **where the condition comes from**. It comes from outside the program when the environment produces it (a missing file, a failed write, the end of an input stream), when it is found in data in an external form that a member is reading into program values (text that does not parse, bytes that do not decode, a document that does not deserialize), when a peer produces it (a closed channel, a failure in another domain), or when a limit does (a deadline, a full buffer). A correct program can meet every one of these, and the condition is a failure. It comes from the program itself when it is a fact about what the program passed (an index, a key, a count, a radix, a format specifier), about the state of an object or collection the program holds (an empty collection, an absent key, a closed scope, a live enumeration), or about its own arithmetic. The program owns that state and can know it before the call, so the condition is a defect, however the values in it were arrived at. The kind is the condition's and not the tier's (the no-dual rule, below): `Parse` and the `[key]` indexer are both throwing forms with a query beside them, and `Parse` raises a failure, because the text is the outside's, where the indexer raises a defect, because the dictionary is the program's. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `SerializationException` ([§2.15](#sec-concrete-serialization-formats)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), `ParseRangeException` ([§9.1](#sec-integer-types)) and `DomainFailedException` ([§6.8](#sec-domain)); with the built-in `FormatException` they are the library's whole failure band. One exception class is of one kind, and a member does not raise a failure with a defect class or a defect with a failure class. No class of this library that implements `IFailure` derives from one that does not, with two recorded exceptions, each keeping an established catch arm matching: `ParseRangeException` derives from `OverflowException`, and `DomainFailedException` from `DomainFaultedException`. A landed member that raises a failure class for a band-1 condition is a recorded misfit under this rule's scoping sentence, flagged where it stands: the unrecognized format specifier ([§2.18](#sec-iformattable), [§2.6](#sec-guid), [§3.1](#sec-stringbuilder)), the write-side serialization conditions ([§2.14](#sec-serialization-vocabulary), [§2.15](#sec-concrete-serialization-formats)) and the unrepresentable code point at `GetBytes` ([§3.2](#sec-encoding)). A kind is inherited and cannot be shed (language spec §31.4), so moving such a condition means raising it with a class outside the failure class's subtree, and the arms naming the failure class stop taking it. A class's kind changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).")

rep("L4", LIB, "§2.5 *Parse and range contracts*",
    "— a band-4 catchable fault, not a band-1 argument defect; the constructors' component validation keeps `ArgumentOutOfRangeException`.",
    "— a band-1 defect of the arithmetic domain ([§1](#sec-failure-signal-rule)), raised with the arithmetic class and not with an argument-blaming one; the constructors' component validation keeps `ArgumentOutOfRangeException`.")

rep("L5", LIB, "§2.3 `Convert`",
    "and `OverflowException` when the value does not fit — the same contract as the §9 per-type `Parse` family these conversions mirror;",
    "and `ParseRangeException` ([§9.1](#sec-integer-types); an `OverflowException`) when the value does not fit — the same contract as the §9 per-type `Parse` family these conversions mirror;")

rep("L6", LIB, "§9.1, *The radix alphabet*",
    "and any other input raises `FormatException`, a value outside the type's range raising `OverflowException`. `ToString(radix)` emits",
    "and any other input raises `FormatException`, a value outside the type's range raising `ParseRangeException` (*The range failure*, below). `ToString(radix)` emits")

ins("L7", LIB, "§9.1, a new paragraph at the end of the section",
    "and share the span forms' per-type, non-constraint status.",
    """

**The range failure.** A `Parse` member — here, in [§9.2](#sec-double-and-float) and [§9.3](#sec-decimal), and through [§2.3](#sec-convert)'s `Convert.To*` — whose input is well-formed and spells a value the type cannot represent raises `ParseRangeException`:

```bezel
public class ParseRangeException: OverflowException, IFailure
  public pure constructor();
  public pure constructor(message: string);
  public constructor(message: string, cause: Exception);
end ParseRangeException;
```

Input from outside the program is a condition a correct program can meet, so the class implements `IFailure` ([§1](#sec-failure-signal-rule)). It derives from `OverflowException` so that an `OverflowException` arm around a `Parse` call matches as it always has; the arithmetic `OverflowException` of language spec [§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic) is a defect, and no arm on `IFailure` takes it. Wherever this document writes *the `FormatException`/`OverflowException` contract* of a `Parse` family, the second class raised is this one. It lives in the auto-imported `System`, beside its producers, and a user type implementing `IParsable<T>` ([§2.19](#sec-static-abstractions)) raises the same two classes.""")

rep("L8", LIB, "§9.2, the grammar paragraph",
    "`\"1_000e999\"` raises `FormatException`, not the `OverflowException` below.",
    "`\"1_000e999\"` raises `FormatException`, not the `ParseRangeException` below.")

rep("L9", LIB, "§9.2, the range paragraph",
    "raises `OverflowException`: the type cannot represent the number the input spells",
    "raises `ParseRangeException` ([§9.1](#sec-integer-types)): the type cannot represent the number the input spells")

rep("L10", LIB, "§9.3 `Decimal`",
    "raises — raises `OverflowException`. `TryParse` returns `false` in every case where `Parse` would raise.",
    "raises — raises `ParseRangeException` ([§9.1](#sec-integer-types)). `TryParse` returns `false` in every case where `Parse` would raise.")

rep("L11", LIB, "§2.15, the declaration of `SerializationException`",
    "public class SerializationException: Exception\n",
    "public class SerializationException: Exception, IFailure\n")

rep("L12", LIB, "§6.1, the declaration of `TimeoutException`",
    "public class TimeoutException: Exception\n",
    "public class TimeoutException: Exception, IFailure\n")

rep("L13", LIB, "§6.9, the two channel exception declarations",
    "public class ChannelClosedException: Exception\nend ChannelClosedException;\n\npublic class ChannelFullException: Exception        // sync Send on a full bounded channel",
    "public class ChannelClosedException: Exception, IFailure\nend ChannelClosedException;\n\npublic class ChannelFullException: Exception, IFailure   // sync Send on a full bounded channel")

rep("L14", LIB, "§7.5 IO exception types",
    "All derive from `IOException`, which derives from `Exception`.",
    "All derive from `IOException`, which derives from `Exception` and implements `IFailure` (language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker)), so every member of the family is a failure.")

rep("L15", LIB, "§6.8, the declaration block",
    "  // Message carries the original exception's message text.\nend DomainFaultedException;\n```",
    "  // Message carries the original exception's message text.\nend DomainFaultedException;\n\npublic class DomainFailedException: DomainFaultedException, IFailure   // the original was a failure\nend DomainFailedException;\n```")

rep("L16", LIB, "§6.8, the paragraph after the declaration",
    "are copied strings. `DomainFaultedException` requires `import System.Threading`,",
    "are copied strings. The exception's kind crosses with them (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): the join raises `DomainFailedException` where the original exception's class implements `IFailure`, and `DomainFaultedException` itself for a defect and for cancellation. `DomainFailedException` declares nothing of its own. It derives from `DomainFaultedException` so that an arm naming that class keeps matching every fault a join raises. Both classes require `import System.Threading`,")

rep("L17", LIB, "§6.12 `Parallel`, *Results in input order*",
    "first fault wins, surfaced at the `await` as `DomainFaultedException` (implementations",
    "first fault wins, surfaced at the `await` as `DomainFaultedException`, or as `DomainFailedException` where that fault is a failure ([§6.8](#sec-domain)) (implementations")

ins("L18", LIB, "§2.17, end of the first paragraph",
    "one that shows constructors carries exactly those.",
    " Which of them are failures (language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker)) is stated once, in [§1](#sec-failure-signal-rule).")

ins("L19", LIB, "§2.18, the specifier-vocabulary paragraph",
    "An unrecognized specifier raises `FormatException` (strict-fault, as everywhere).",
    " The condition is a band-1 defect raised with a failure class: a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, shared by `Guid.ToString(format)` ([§2.6](#sec-guid)) and `StringBuilder.AppendFormat` ([§3.1](#sec-stringbuilder)), which stands until its own ruling.")

ins("L20", LIB, "§2.14, the registry paragraph",
    "raises `SerializationException` — the closed set enforced on both directions.",
    " (The write-side half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, as a `Weak` reference to a target never strongly reached is.)")

rep("L21", LIB, "§1, the Try convention, *the three access tiers*",
    "the **throwing form** (`Parse`, the `[key]` indexer) treats absence as a bug, the **`*OrNull` form** returns",
    "the **throwing form** (`Parse`, the `[key]` indexer) asserts the value is there and raises where it is not (a failure or a defect by where the condition comes from — *Failures and defects*, below), the **`*OrNull` form** returns")

rep("L22", LIB, "§1, the no-dual rule's gloss of the two tiers",
    "the assert-shaped throwing access (\"failure is a bug at this call site\") beside the branch-shaped value access (\"failure is expected here\"), the [§9.1](#sec-integer-types) `Parse`/`TryParse` pair being the model — because tiers serve different call-site *stances* toward one operation, not the same problem twice.",
    "the assert-shaped throwing access (\"this call site handles nothing: it asserts the operation succeeds, and an exception propagates if it does not\") beside the branch-shaped value access (\"this call site handles the outcome\"), the [§9.1](#sec-integer-types) `Parse`/`TryParse` pair being the model — because tiers serve different call-site *stances* toward one operation, not the same problem twice. A stance is the caller's. It does not make the exception a failure or a defect, which is the condition's (*Failures and defects*, above): the throwing `Parse` raises a failure and the throwing indexer a defect.")

rep("L23", LIB, "§2.1 `Console`, the `ReadKey` paragraph",
    "`ReadKey` remains the assert-shaped throwing tier — end of input is a bug at its call sites.",
    "`ReadKey` remains the assert-shaped throwing tier: a caller that does not expect end of input handles nothing, and the `EndOfStreamException` then raised is a failure ([§1](#sec-failure-signal-rule)), end of input being the environment's and not the program's.")

ins("L24", LIB, "§3.2 `Encoding`, the defined-fault paragraph",
    "`GetBytes` raises `FormatException` when the string contains a code point the encoding cannot represent (possible only for `ASCII`).",
    " (The `GetBytes` half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule: the program chose an encoding that cannot represent its own text.)")

# ------------------------------------------------------------------ rationale
ins("R1", RAT, "§11, a new section §11.12 after §11.11 (before `## 12. Async`)",
    "<a id=\"sec-async\"></a>",
    None)  # filled below: the new section is inserted BEFORE this anchor

R1_TEXT = """<a id="sec-failures-and-defects"></a>
### 11.12 Failures and defects: the kind is a marker interface on the class

(Proposal 0211.) The failure-signal rule (§11.4) separates a caller's bug from a failure consumed at a boundary. The difference is a fact about the exception's class — it implements `IFailure` or it does not (spec §31.4) — so that a handler can name it: the boundary arm names the marker, where an arm on `Exception` takes both.

**An interface, not a base class.** A base class `Failure: Exception` is the simpler thing to say and needs no rule at the catch arm. It was declined on three grounds. A class has one base, so a band drawn as a subtree cannot hold a class that must also stay under another: the out-of-range `Parse` input, which existing `OverflowException` arms take, and the fault a domain join raises, which existing `DomainFaultedException` arms take. Each would have had to leave its base, and the arms written against that base would have stopped matching. Second, moving a class between bands would be a change of base class, which spec Appendix E classifies as breaking for every consumer, where adding an interface touches only the arms that name it. Third, the precedent is against the subtree: Java drew its checked/unchecked line by base class and filed `NumberFormatException` — malformed input, the plainest failure there is — under the unchecked `IllegalArgumentException`, because its is-a parent was on the other side of the line. A kind that cuts across the is-a tree needs a mechanism that can.

**The cost of the interface, and the rule that pays it.** A binding of interface type exposes only the interface's members, so a boundary arm that bound the marker could not read `Message`, pass the exception to a logger, or chain it as a cause — the three things a boundary arm does. Declaring the reporting members on the marker fixes the first and not the other two. The rule taken is local to the catch arm and general: an interface in an arm selects, and the binding is the exception, typed at the most specific class the arm's class types determine (spec §31.2.1). It is the rule the multi-type alternative already had for classes, extended to the one case its text did not cover.

**A recorded difference from `case`.** [§11.3](#sec-catch-is-case-for-exceptions) says that `catch` arms mirror `case` arms, and in a `case` a type pattern's binding has the pattern's written type (spec §19.3), so `case e of var f: IFailure` binds an `IFailure`. A `catch` arm over the same pattern binds an `Exception`. The two already differed for the multi-type alternative, whose binding is the nearest common base rather than any written type, and for the same reason: a `case` scrutinee may be anything, a caught value is known to be an `Exception`, and a handler's work is done on the exception. Keeping the mirror exact was weighed; it leaves the marker arm unable to log or wrap, which is what the arm is for.

**Why the marker has no members, and who may carry it.** With the binding an `Exception`, a member on the marker would be a second way to reach what the binding already has. Conformance is restricted to `Exception`-derived classes on the ground §11.7 used for arm types: a record or a non-exception class that lists the marker is a shape no program can observe, and Bezel rejects dead shapes rather than carrying them. A second restriction keeps the documents' own classification true: a class under a built-in or standard-library class that is not a failure may not list the marker. Without it `class C: OperationCanceledException, IFailure` would make a failure arm take a cancellation, and a user's failure under `InvalidOperationException` would be taken by every state-defect arm — the swallowing stdlib §4.8 keeps `KeyNotFoundException` clear of. The library's two bridging classes are the only ones across that line, each recorded with its reason; a user hierarchy under a user base stays free to mix kinds, since no arm on a class of the documents is affected.

**Unmarked means defect.** A user exception class is a defect report until it lists the marker. The other default would let a boundary arm swallow whatever a library author forgot to classify, which is the failure this entry exists to remove; the cost of the chosen default is one interface name on each class meant to be handled.

**What the marker does not do.** It forces nothing: no `throws` clause, no obligation to handle (spec §31.3). It changes no ownership rule, and bugs still unwind and clean up (spec K3.15). Abandoning the process or the domain on a defect was weighed and declined: it would skip the cleanup the exception edge guarantees, and it would make a contract violation uncatchable, where spec §22.16 makes it a catchable exception and the test oracle and the supervisor both depend on catching one. It leaves the query/assert tiers alone: a missing key and an empty collection stay defects in their throwing forms, because each has a query beside it.

**Reopening bars.** For the base class: evidence that the catch-arm binding rule misleads readers, together with a migration for the two bridging classes. For re-typing a query's throwing form as a failure: a declared failure surface on members (a `fails` clause), which would let the pairs be derived rather than hand-written.

"""
E[-1]["add_before"] = R1_TEXT

rep("R2", RAT, "§12.6, *Faults cross as `DomainFaultedException`*",
    "Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value;",
    "Copying the message and stack-trace text (strings — sendable) preserves the diagnostic value, and the class the join raises carries the one fact a handler selects on — failure or not ([§11.12](#sec-failures-and-defects)) — with no exception class becoming sendable;")

ins("R3", RAT, "§11.4, end of the last paragraph",
    "and one mechanism per fault condition — chosen per condition by the rule, not per language by fashion.",
    " The line between the rule's first and fourth bands is visible in the exception's class ([§11.12](#sec-failures-and-defects)).")

# ------------------------------------------------------------------ Guide 2
rep("G2-1", G2, "the opening list",
    "- A **caller bug** is a contract (`requires`, §7) or defect exception.\n- A **boundary fault** — I/O, network, anything logged or retried rather than branched on — is a typed exception (§4).",
    "- A **caller bug** is a contract (`requires`, §7) or a defect exception: a report, not something to handle.\n- A **failure** — I/O, network, bad input: anything a correct program can meet, logged or retried rather than branched on — is a typed exception whose class implements `IFailure` (§4).")

rep("G2-2", G2, "§4, the opening sentence",
    "For everything the value shapes don't cover — the boundary fault, and the defect a `requires` contract (§7) cannot state — the exception machinery is the familiar one.",
    "For everything the value shapes don't cover — the failure consumed at a boundary, and the defect a `requires` contract (§7) cannot state — the exception machinery is the familiar one.")

ins("G2-3", G2, "§4, new paragraphs after the hierarchy paragraph",
    "Order arms most-specific first.",
    """

**Failures and defects.** Every exception is one of two kinds, and its class says which (spec §31.4). A *failure* is something a correct program can meet — a missing file, malformed input, a peer that went away, a deadline — and its class implements the marker interface `IFailure`. Everything else is a *defect* report: an index out of range, a broken contract, a null where none was promised. `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException` and the channel exceptions are failures; `ArgumentException`, `InvalidOperationException`, `IndexOutOfRangeException`, `OverflowException`, `ContractViolationException` and their relatives are defects. (`OperationCanceledException` is neither: it answers a cancellation you asked for.)

That gives a boundary its arm. Where you would have reached for `Exception`, name the marker:

```bezel
method Serve(request: Request): Response
  try
    return Dispatch(request);
  catch
    var f: IFailure => do                    // expected: answered
      return Response.BadGateway(f.Message);
    end,
    var ex: Exception => do                  // a bug: reported, not hidden
      ReportDefect(ex);
      return Response.InternalError();
    end
  end;
end Serve;
```

The `IFailure` arm takes every failure and lets every bug through; `f` is an ordinary `Exception` inside the arm. The second arm is there only because this boundary is a supervisor that must keep serving. Most boundaries have no second arm: a bug then propagates, and an uncaught one ends the program with its report (spec §33.3). Where cancellation can reach a supervisor, give it its own arm ahead of the `Exception` arm — `OperationCanceledException => throw` — so that it is not reported as a bug. One arm on bare `Exception` that handles both is the habit to drop: it answers a bug as if it were bad input. An exception class of your own is a defect until you say otherwise:

```bezel
public class ConfigError(message: string): Exception(message), IFailure
end ConfigError;
```

Two habits keep the kinds honest. Wrap a failure in a failure and never wrap a defect: re-raise it. And when out-of-range text matters, catch `FormatException` and `ParseRangeException` (or just `IFailure`); `ParseRangeException` is the failure `int.Parse("99999999999")` raises, distinct from the arithmetic `OverflowException` of `a * b`, which is a defect.""")

rep("G2-4", G2, "§5, the `Handle` example",
    "  catch\n    var ex: Exception => RecordError(ex)\n  end;",
    "  catch\n    var ex: IFailure => RecordError(ex)   // ex is an Exception here; a bug propagates\n  end;")

rep("G2-5", G2, "§7, the boundary row of the table",
    "| a typed exception, producer-local (spec §31.4); read it in the `catch` and copy out what you keep (§5) |",
    "| a typed exception whose class implements `IFailure`, producer-local (spec §31.4); catch it at the boundary with an `IFailure` arm and copy out what you keep (§4, §5) |\n| Handling whatever goes wrong at a boundary | an `IFailure` arm; a separate `Exception` arm only in a supervisor that reports the bug, never one arm for both (§4) |")

ins("G2-6", G2, "§7, the paragraph after the table",
    "Branching program logic on caught exception types at an interior call site is the signal to check the rule for a value-shaped tier.",
    " And at the boundary the arm names `IFailure`: an arm on bare `Exception` takes the bugs too.")

# ------------------------------------------------------------------ Guide 4
rep("G4-1", G4, "§4, the `Parallel` paragraph",
    "First fault cancels remaining work and surfaces at the await as `DomainFaultedException` (stdlib §6.12).",
    "First fault cancels remaining work and surfaces at the await as `DomainFaultedException` — as its subclass `DomainFailedException` when that fault was a failure, so that an `IFailure` arm takes it (stdlib §6.12).")

ins("G4-2", G4, "§5, the paragraph on joins",
    "A fault in the entry surfaces at the join as `DomainFaultedException` carrying the original message and stack-trace *text* — exception objects themselves do not cross (spec §30.14).",
    " Its kind does cross: a failure in the child is joined as `DomainFailedException`, a `DomainFaultedException` that an `IFailure` arm also takes, and a bug in the child is joined as the plain class, which that arm lets pass (Guide 2 §4).")

# ------------------------------------------------------------------ Guide 6
rep("G6-1", G6, "§2, the paragraph before the `Listen` example",
    "the sanctioned signal is a typed exception (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).",
    "the sanctioned signal is a typed exception whose class implements `IFailure`: a failure (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).")

rep("G6-2", G6, "§2, the comment in the `Listen` example",
    "    // Boundary-consumed expected failure: a typed exception, still never\n    // a requires. FormatException is the built-in format fault (spec §31.4).",
    "    // Boundary-consumed expected failure: a typed exception, still never\n    // a requires. FormatException is the built-in input failure: its class\n    // implements IFailure (spec §31.4).")

rep("G6-3", G6, "§2, the paragraph after the `Listen` example",
    "Why not just call the throwing `int.Parse(rawPort)` and let its exception fly, if you were going to throw anyway? Two reasons. The throwing form is the *assert-shaped* tier — stdlib §1's words are that it \"treats absence as a bug\" — so calling it on boundary input re-asserts, in exception spelling, the same misclassification the bad `requires` made. And it splits the condition: `int.Parse` raises `FormatException` for a malformed numeral but `OverflowException` for an out-of-range one (stdlib §9.1, §2.3), while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The same reasoning rules out `ArgumentException`:",
    "Why not just call the throwing `int.Parse(rawPort)` and let its exception fly, if you were going to throw anyway? You may. What it raises is a failure either way — `FormatException` for a malformed numeral, `ParseRangeException` for an out-of-range one (stdlib §9.1, §2.3) — because the text came from outside the program, and a boundary's `IFailure` arm takes both (Guide 2 §4). The explicit throw is for the wording: `int.Parse` reports two conditions in the numeral's terms, while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The kind is also what rules out `ArgumentException`:")

ins("G6-4", G6, "§2, the two-question paragraph",
    "a typed exception, as in the `Listen` rewrite (Guide 2 §4).",
    " The kinds follow the answer: a broken `requires` raises `ContractViolationException`, a defect report that a boundary's `IFailure` arm lets pass, and the typed exception for bad input is a failure, which that arm takes (spec §31.4).")

# ------------------------------------------------------------------ Tour
rep("T1", TOUR, "§6 *Errors and nullability*, the exceptions paragraph",
    "**Exceptions** are for the unexpected. A `catch` has comma-separated arms, matched in order:",
    "**Exceptions** are for the unexpected, and are of two kinds: a *failure* (a missing file, bad input), whose class implements `IFailure`, and a *defect* report, which is a bug. `IFailure => …` is the arm that takes every failure and no bug. A `catch` has comma-separated arms, matched in order:")

# ------------------------------------------------------------------ code-generation reference
rep("C1", CG, "§5.1, *Three access tiers over one operation*",
    "the **throwing** tier (absence is a bug) — `Parse`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`, `Console.ReadKey`;",
    "the **throwing** tier (the call asserts the value is there and raises otherwise: a *failure* where the condition is the outside's — `Parse`, `Console.ReadKey` — and a *defect* where it is the program's own state — `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`);")

rep("C2", CG, "§5.2, the `Convert` listing",
    "end                                                    // faults: FormatException, OverflowException",
    "end                                                    // failures: FormatException, ParseRangeException")

ins("C3", CG, "§5.2, the exception hierarchy line",
    "`FormatException`, `StackOverflowException` (uncatchable).",
    " **Kinds (§31.4):** a class that implements the member-less interface `IFailure` is a *failure* — `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException`, `ChannelClosedException`, `ChannelFullException`, `ParseRangeException` (an `OverflowException`: out-of-range `Parse`/`Convert.To*` input) and `DomainFailedException` (a `DomainFaultedException`: a failure joined from another domain); every other is a *defect* report; `OperationCanceledException` is neither. Only an `Exception`-derived class may list `IFailure`, and not one under a built-in or stdlib class that is not itself a failure.")

rep("C4", CG, "§5.6, the `Domain` listing",
    "end                                   // the entry MUST be capture-free; EXACTLY ONE argument",
    "end                                   // the entry MUST be capture-free; EXACTLY ONE argument\n                                      // a fault in the entry is joined as DomainFaultedException; as\n                                      //   DomainFailedException (a subclass, IFailure) when it was a failure")

rep("C5", CG, "§5.9, the integer `Parse` line",
    "out of range `OverflowException`;",
    "out of range `ParseRangeException` (an `OverflowException`; a failure);")

rep("C6", CG, "§5.9, the `Double`/`Float` `Parse` line",
    "raises `OverflowException` exactly where the same digits as a literal of that type are out of range",
    "raises `ParseRangeException` exactly where the same digits as a literal of that type are out of range")

rep("C7", CG, "§6, *Declaring, throwing and catching an exception type*",
    "Derive from `Exception` (or any built-in exception type) and chain the message to the base; a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.\n\n```bezel\npublic class ConfigError(message: string): Exception(message)\nend ConfigError;",
    "Derive from `Exception` (or any built-in exception type) and chain the message to the base; list `IFailure` when the condition comes from outside the program, since an unmarked class is a defect report (§31.4); a caught exception is a **borrow** valid inside its arm only, so copy out what you keep.\n\n```bezel\npublic class ConfigError(message: string): Exception(message), IFailure\nend ConfigError;")

ins("C8", CG, "§6, a new idiom after the exception-wrapping paragraph",
    "never retained (§31.4); there is no user `InnerException`.",
    """

**Catching at a boundary.** Name the marker, not `Exception`: an `IFailure` arm takes every failure and lets every bug through, and its binding is an ordinary `Exception` (§31.2.1).

```bezel
try
  Serve(request);
catch
  var f: IFailure => Respond(502, f.Message),                  // failures: answered
  var ex: Exception => do ReportDefect(ex); Respond(500); end  // a supervisor only: bugs reported
end;
```

Leave the second arm out unless the code is a supervisor: a bug should propagate. `IFailure => ()` suppresses failures only. To reach an interface's own members inside an arm, test in the body: `var r := ex as IRetryable;`.""")


def load(root):
    return {d: open(os.path.join(root, d), encoding="utf-8").read() for d in {e["doc"] for e in E}}

def apply_all(texts):
    bad = 0
    for e in E:
        t = texts[e["doc"]]; n = t.count(e["old"])
        if n != 1:
            print(f"{e['id']}: `old` occurs {n} times in {e['doc']}"); bad += 1; continue
        if e["kind"] == "replace": new = e["new"]
        elif "add_before" in e: new = e["add_before"] + e["old"]
        else: new = e["old"] + e["add"]
        texts[e["doc"]] = t.replace(e["old"], new)
    return bad

def new_text(e):
    return e["new"] if e["kind"] == "replace" else e.get("add_before", e.get("add"))

def render():
    names = {SPEC: "Language specification", LIB: "Standard library", RAT: "Design rationale",
             G2: "Guide 2", G4: "Guide 4", G6: "Guide 6", TOUR: "Tour", CG: "Code-generation reference"}
    cur = None
    for e in E:
        if e["doc"] != cur:
            cur = e["doc"]; print(f"\n#### {names[cur]} (`{cur}`)\n")
        verb = "replace" if e["kind"] == "replace" else ("insert before" if "add_before" in e else "insert after")
        print(f"**{e['id']}** — {e['where']}: {verb}\n")
        print("````text"); print(e["old"]); print("````\n")
        print("with\n" if e["kind"] == "replace" else "the text\n")
        print("````text"); print(new_text(e).strip("\n")); print("````\n")

if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "render":
        render()
    elif cmd == "twin":
        doc = open(sys.argv[2], encoding="utf-8").read(); bad = 0
        for e in E:
            for part in (e["old"], new_text(e).strip("\n")):
                if part not in doc:
                    print(f"{e['id']}: text not found verbatim in the proposal"); bad += 1
        print(f"{len(E)} edits; {bad} mismatches"); sys.exit(1 if bad else 0)
    else:
        root = sys.argv[2]; texts = load(root); bad = apply_all(texts)
        print(f"{len(E)} edits over {len(texts)} documents; {bad} anchor failures")
        if cmd == "apply" and not bad:
            out = sys.argv[3]
            for d, t in texts.items():
                p = os.path.join(out, d); os.makedirs(os.path.dirname(p), exist_ok=True)
                open(p, "w", encoding="utf-8").write(t)
            print("patched documents written under", out)
        sys.exit(1 if bad else 0)
