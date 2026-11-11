#!/usr/bin/env python3
"""The normative edit set of proposal 0214 (the narrowed failure marker), machine-checkable.

Each edit names a document, an `old` text that must occur exactly once in the document as
the edits before it leave it, and the `new` text that replaces it.

  edits_0214.py check <repo-root>     every `old` occurs exactly once, in sequence; nothing is written
  edits_0214.py apply <repo-root>     apply the edits in place (the v0.358 landing)
  edits_0214.py render                print the edits as proposal 0214 section 9 prints them
  edits_0214.py landed <repo-root>    every `new` text occurs in the documents (after landing)

Proposal 0214 section 9 is the authority; this file is its twin, so that the anchors can be
re-verified against HEAD. Written against v0.357 (tree e49c32aca).
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
def rep(i, doc, where, old, new):
    E.append(dict(id=i, doc=doc, where=where, old=old, new=new))

# ------------------------------------------------------------------ spec
rep("S1", SPEC, "§30.14 *Faults*",
    "The exception's kind crosses with them: where the original exception's class implements `IFailure`",
    "The failure marker crosses with them: where the original exception's class implements `IFailure`")

rep("S2", SPEC, "§31.1 *The throw operand*, the last two sentences",
    "A throwable whose class implements `IFailure` is a **failure**; every other is a **defect** report, cancellation apart ([§31.4](#sec-the-failure-marker)). Dispatch reads the kind only where an arm names the marker.",
    "A throwable whose class implements `IFailure` is a **failure**: the environment or a peer produced it ([§31.4](#sec-the-failure-marker)). A class that does not implement it makes no statement about where its condition came from. Dispatch reads the marker only where an arm names it.")

rep("S3", SPEC, "§31.2.4, the last example's comment",
    "  var f: IFailure => Log(f)                 // every other failure; `f` is an `Exception` (§31.2.1); defects pass",
    "  var f: IFailure => Log(f)                 // what the environment or a peer raised; `f` is an `Exception` (§31.2.1); the rest propagates")

rep("S4", SPEC, "§31.2.5, the last sentence",
    "To suppress expected failures only, name the marker — `IFailure => ()` — and defects and cancellation still propagate ([§31.4](#sec-the-failure-marker)).",
    "To suppress only what the environment or a peer raised, name the marker — `IFailure => ()` — and every other exception, cancellation included, still propagates ([§31.4](#sec-the-failure-marker)).")

rep("S5", SPEC, "§31.4, the first paragraph",
    "(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms, and the `()` and `(message)` forms of `ParseRangeException`, stdlib [§9.1](Bezel_Standard_Library.md#sec-integer-types)) are **`pure`**",
    "(and `ArgumentException`/`ArgumentOutOfRangeException`'s `(message, paramName)` forms) are **`pure`**")

rep("S6", SPEC, "§31.4, the tree's `FormatException` line",
    "├─ FormatException                  — string does not conform to an expected format. A failure: implements `IFailure`.",
    "├─ FormatException                  — string does not conform to an expected format.")

rep("S7", SPEC, "§31.4 *The failure marker*, the first paragraph",
    "**The failure marker.** An exception is of one of two kinds, and the kind is a property of its class. A **failure** reports a condition a correct program can meet: the environment, input from outside the program, a peer, or a time or capacity limit produced it. Its class implements the built-in interface `IFailure`. Every other exception is a **defect** report: the program is wrong, and the exception says where. One class is named apart: `OperationCanceledException` reports **cancellation**, the answer to a request the program itself made; it does not implement `IFailure`, and a handler of failures never takes it.",
    "**The failure marker.** An exception whose class implements the built-in interface `IFailure` is a **failure**: the outside world disagreed. The environment produced the condition (a file system, a device, an input stream, the operating system), or a peer did (the other end of a channel, a connection, another domain), or the outside did not answer in time (a deadline). That is a statement of origin, which the class's author can make, and a default for handlers. It is not a disposition: whether a program carries on, reports, retries or stops when a failure reaches it is the decision of the handler that takes it, and a failure the program cannot do without — a file it was installed with, the server it exists to call — is still a failure, and the program answers it by stopping. A class that does not implement `IFailure` makes **no statement**: its condition may come from the program, from data the program was given, or from anywhere else, and only the code that handles it knows which. One class is named apart: `OperationCanceledException` reports **cancellation**, the answer to a request the program itself made; no class in its tree implements `IFailure`, and a handler of failures never takes it.")

rep("S8", SPEC, "§31.4 *The failure marker*, the second paragraph",
    "so a kind is inherited and cannot be shed.",
    "so the marker is inherited and cannot be shed.")

rep("S9", SPEC, "§31.4 *Who may conform*, after the first restriction",
    "And a class does not contradict the kind of a class the documents declare: a class that does not inherit `IFailure` may list it only where no class in its base chain, other than `Exception` itself, is a built-in or standard-library exception class; listing it otherwise is a compile error at the declaration. So a class deriving from `OperationCanceledException` is cancellation in every subclass, and an arm naming `InvalidOperationException`, or any other defect class of the documents, never takes a failure that user code declared. A class whose base chain is user-declared down to `Exception` chooses freely at each level. The standard library declares the only two classes that derive across that line, each by recorded ruling (stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule)). An interface may extend `IFailure`; both restrictions then bind its implementers.",
    "And a class with `OperationCanceledException` in its base chain may not implement it: listing `IFailure`, or an interface extending it, on such a class is a compile error at the declaration, so cancellation is cancellation in every subclass. Every other `Exception`-derived class may list it, whatever its base: a class deriving from `InvalidOperationException`, from `FormatException` or from a class of the program's own is a failure where its author says so, and an arm naming its base still takes it. An interface may extend `IFailure`; both restrictions then bind its implementers.")

rep("S10", SPEC, "§31.4, the paragraph after *Who may conform*",
    "Of the built-in types, `FormatException` implements `IFailure`. The standard library's failure types, and the rule by which a condition is raised as a failure or as a defect, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). A class that this specification or the standard library declares changes kind only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker declares a defect report.",
    "No built-in type implements `IFailure`. The standard library's failure types, and the rule by which the library marks a class, are stated in stdlib [§1](Bezel_Standard_Library.md#sec-failure-signal-rule). Whether a class this specification or the standard library declares implements the marker changes only by recorded ruling. User code chooses per class: `class ConfigError(message: string): Exception(message), IFailure` declares a failure, and a class that does not list the marker makes no statement.")

rep("S11", SPEC, "§31.4, the paragraph on what reads the marker",
    "The kind is read in two places. A catch arm that names the marker selects on it: an `IFailure` arm takes every failure and lets defects and cancellation pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)).",
    "The marker is read in two places. A catch arm that names it selects on it: an `IFailure` arm takes every failure and lets every other exception, cancellation included, pass ([§31.2.3](#sec-catch-ordering-and-exhaustiveness)).")

rep("S12", SPEC, "§31.4, the same paragraph, its last sentence",
    "and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for both kinds and for cancellation.",
    "and the reports of [§32.8](#sec-callbacks) and [§33.3](#sec-exit-code) are the same for a failure, for an exception whose class makes no statement, and for cancellation.")

rep("S13", SPEC, "§31.4, the last paragraph (the stdlib exception types)",
    "`ParseRangeException` (deriving from `OverflowException` and implementing `IFailure`; raised by the `Parse` family for a value outside the type's range; declared in the auto-imported `System`, stdlib §9.1), ",
    "")

rep("S14", SPEC, "Appendix C.7, the `IFailure` bullet",
    "and so is a class that does not inherit it and has a built-in or standard-library exception class other than `Exception` in its base chain — `OperationCanceledException` and every defect class among them ([§31.4](#sec-the-failure-marker)).",
    "and so is a class with `OperationCanceledException` in its base chain ([§31.4](#sec-the-failure-marker)).")

rep("S15", SPEC, "Appendix E, the `IFailure` row",
    "The kind is what a consumer's boundary arm selects on ([§31.4](#sec-the-failure-marker)): adding the marker makes every `IFailure` arm take the class.",
    "The marker is what a consumer's `IFailure` arm selects on ([§31.4](#sec-the-failure-marker)): adding it makes every such arm take the class.")

# ------------------------------------------------------------------ stdlib
rep("L1", LIB, "§1, the three access tiers",
    "asserts the value is there and raises where it is not (a failure or a defect by where the condition comes from — *Failures and defects*, below)",
    "asserts the value is there and raises where it is not (the class raised is the condition's, whatever the caller's stance — *The failure marker*, below)")

rep("L2", LIB, "§1, band 1",
    "the residual defect exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries, and the arithmetic classes of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types) (`OverflowException`, `DivideByZeroException`) where a member surfaces an arithmetic fault. A defect surface is never a recovery surface: its exception exists to report a bug, not to be branched on, and its class does not implement `IFailure` (*Failures and defects*, below).",
    "the residual exceptions (such as `Argument*` and `InvalidOperationException`) at pre-contract boundaries, and the arithmetic classes of language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types) (`OverflowException`, `DivideByZeroException`) where a member surfaces an arithmetic fault. The band fixes what the library offers, not what a handler does: a caller that expects the condition at its call tests first, with the query or tester form beside the operation where one exists, and a handler that catches one of these exceptions decides what it means, as for every exception. None of these classes implements `IFailure` (*The failure marker*, below).")

rep("L3", LIB, "§1, band 4",
    "→ a typed exception whose class implements `IFailure` (a **failure**; *Failures and defects*, below), producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**, with the classification and its reason recorded beside the surface. The typical inhabitant is the environmental fault (I/O, network, OS state). What separates this band from band 1 is where the condition comes from, the test stated once in *Failures and defects* (below). This band's wording about where the fault is consumed does not replace that test.",
    "→ a typed exception, producer-local per language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types), **regardless of how much information it carries**, with the classification and its reason recorded beside the surface; its class implements `IFailure` where the environment or a peer produces the condition (*The failure marker*, below). The typical inhabitant is the environmental fault (I/O, network, OS state); environmental origin describes the band, it is not a conjunct of the test.")

rep("L4", LIB, "§1, *Failures and defects*, replaced by *The failure marker*",
    "**Failures and defects.** The exceptions this library raises are of the kinds language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) defines, and the band fixes the kind: a band-4 condition is raised with a class that implements `IFailure`, a band-1 condition with one that does not, and cancellation with `OperationCanceledException`. Bands 2 and 3 have value shapes as their mechanism; where a member also offers a throwing tier (the no-dual rule, below), the throwing tier of a band-2 or band-3 condition takes the kind its origin gives: end of input at `Console.ReadKey` is the environment's, and a failure; an absent key at the indexer is the program's, and a defect. The test, between bands 1 and 4 and for every throwing tier, is **where the condition comes from**. It comes from outside the program when the environment produces it (a missing file, a failed write, the end of an input stream), when it is found in data in an external form that a member is reading into program values (text that does not parse, bytes that do not decode, a document that does not deserialize), when a peer produces it (a closed channel, a failure in another domain), or when a limit does (a deadline, a full buffer). A correct program can meet every one of these, and the condition is a failure. It comes from the program itself when it is a fact about what the program passed (an index, a key, a count, a radix, a format specifier), about the state of an object or collection the program holds (an empty collection, an absent key, a closed scope, a live enumeration), or about its own arithmetic. The program owns that state and can know it before the call, so the condition is a defect, however the values in it were arrived at. Two lines are drawn once, here. What makes data the outside's is its form: text or bytes that a member is reading into a program value. A conversion from one program value to another — an enum cast, a checked narrowing — tests a value the program already holds, and is the program's. And the other end of a channel is a peer even within one domain: a sender does not hold the receiving end's state, nor the receiver the senders', so `ChannelClosedException` is a failure whichever party closed the channel; a `TaskScope` has one owner, and spawning into a scope that owner has closed is a fact about state it holds. The kind is the condition's and not the tier's (the no-dual rule, below): `Parse` and the `[key]` indexer are both throwing forms with a query beside them, and `Parse` raises a failure, because the text is the outside's, where the indexer raises a defect, because the dictionary is the program's. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `SerializationException` ([§2.15](#sec-concrete-serialization-formats)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), `ParseRangeException` ([§9.1](#sec-integer-types)) and `DomainFailedException` ([§6.8](#sec-domain)); with the built-in `FormatException` they are the library's whole failure band. One exception class is of one kind, and a member does not raise a failure with a defect class or a defect with a failure class. No class of this library that implements `IFailure` derives from one that does not, with two recorded exceptions, each keeping an established catch arm matching: `ParseRangeException` derives from `OverflowException`, and `DomainFailedException` from `DomainFaultedException`. A landed member that raises a failure class for a band-1 condition is a recorded misfit under this rule's scoping sentence, flagged where it stands: the unrecognized format specifier ([§2.18](#sec-iformattable), [§2.6](#sec-guid), [§3.1](#sec-stringbuilder)), the write-side serialization conditions ([§2.14](#sec-serialization-vocabulary), [§2.15](#sec-concrete-serialization-formats)) and the unrepresentable code point at `GetBytes` ([§3.2](#sec-encoding)). A kind is inherited and cannot be shed (language spec §31.4), so moving such a condition means raising it with a class outside the failure class's subtree, and the arms naming the failure class stop taking it. A class's kind changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)).",
    "**The failure marker.** The exceptions this library raises carry the marker of language spec [§31.4](Bezel_Language_Spec.md#sec-the-failure-marker) by **origin**. A class implements `IFailure` exactly when its condition is the outside world disagreeing: the **environment** produced it (a missing file, a refused permission, a failed write, the end of an input stream), or a **peer** did (the other end of a channel closed it or let its buffer fill — a peer even within one domain, whichever party closed the channel; a computation in another domain failed), or the outside did not answer in time (a deadline). A class whose condition can come from the program itself, or from data whose supplier the library cannot see, is not marked, and makes no statement: text that does not parse and a document that does not deserialize may be a user's typing or the program's own constant (`FormatException`, `SerializationException`); an argument, the state of an object the program holds and the program's arithmetic are the program's (`ArgumentException`, `InvalidOperationException`, `KeyNotFoundException`, `OverflowException`). The marker is a default for handlers and never a disposition. Whether a program carries on, reports, retries or stops when a failure reaches it is the handler's decision — a missing file the user chose and a missing database file the program was installed with raise the same class, and only the program knows which one it can do without — and an unmarked exception that a handler expects, a `FormatException` from text a user typed among them, is handled by naming its class. The failure types this document declares are `IOException` and the classes deriving from it ([§7.5](#sec-io-exception-types)), `TimeoutException` ([§6.1](#sec-cancellationtoken-and-cancellationtokensource)), `ChannelClosedException` and `ChannelFullException` ([§6.9](#sec-channel)), and `DomainFailedException` ([§6.8](#sec-domain)), which carries a failure raised in another domain across the join; no built-in class is one. Bands 2 and 3 have value shapes as their mechanism; where a member also offers a throwing tier (the no-dual rule, below), that tier's class takes the marker its origin gives: end of input at `Console.ReadKey` is the environment's, and `EndOfStreamException` is a failure. Whether a class this document declares implements the marker changes only by recorded ruling, as the taxonomy grows only by one (language spec [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)); a class a later revision adds states its marker by the same test, the network family among them.")

rep("L5", LIB, "§1, the no-dual rule",
    "A stance is the caller's. It does not make the exception a failure or a defect, which is the condition's (*Failures and defects*, above): the throwing `Parse` raises a failure and the throwing indexer a defect.",
    "A stance is the caller's. It does not mark the exception, and the marker does not choose the stance (*The failure marker*, above): the throwing `Parse` raises `FormatException` whoever supplied the text.")

rep("L6", LIB, "§2.3, the `To*` contract",
    "and `ParseRangeException` ([§9.1](#sec-integer-types); an `OverflowException`) when the value does not fit",
    "and `OverflowException` when the value does not fit")

rep("L7", LIB, "§2.5 *Parse and range contracts*",
    "a band-1 defect of the arithmetic domain ([§1](#sec-failure-signal-rule))",
    "a band-1 condition of the arithmetic domain ([§1](#sec-failure-signal-rule))")

rep("L8", LIB, "§2.6, the `ToString(format)` sentence",
    "`ToString(format)` raises `FormatException` for a specifier outside its vocabulary. The specifier is the program's argument, so the condition is a band-1 defect raised with a failure class. It is the recorded misfit that [§2.18](#sec-iformattable) states under [§1](#sec-failure-signal-rule)'s rule, and it stands until its own ruling.",
    "`ToString(format)` raises `FormatException` for a specifier outside its vocabulary ([§2.18](#sec-iformattable)).")

rep("L9", LIB, "§2.14, the registry paragraph",
    " (The write-side half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, as a `Weak` reference to a target never strongly reached is.)",
    "")

rep("L10", LIB, "§2.15, the `SerializationException` declaration",
    "public class SerializationException: Exception, IFailure",
    "public class SerializationException: Exception")

rep("L11", LIB, "§2.18, the specifier vocabulary",
    " The condition is a band-1 defect raised with a failure class: a recorded misfit under [§1](#sec-failure-signal-rule)'s rule, shared by `Guid.ToString(format)` ([§2.6](#sec-guid)) and `StringBuilder.AppendFormat` ([§3.1](#sec-stringbuilder)), which stands until its own ruling.",
    "")

rep("L12", LIB, "§3.1, *Contracts*",
    " The format string is the program's argument, so each of these conditions is a band-1 defect raised with a failure class. They are recorded misfits under [§1](#sec-failure-signal-rule)'s rule, with the specifier of [§2.18](#sec-iformattable), and they stand until their own ruling.",
    "")

rep("L13", LIB, "§3.2, *Semantics of the standard encodings*",
    " (The `GetBytes` half is a band-1 defect raised with a failure class, a recorded misfit under [§1](#sec-failure-signal-rule)'s rule: the program chose an encoding that cannot represent its own text.)",
    "")

rep("L14", LIB, "§6.8, after the declarations",
    "The exception's kind crosses with them (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): the join raises `DomainFailedException` where the original exception's class implements `IFailure`, and `DomainFaultedException` itself for a defect and for cancellation.",
    "The failure marker crosses with them (language spec [§30.14](Bezel_Language_Spec.md#sec-multiple-domains)): the join raises `DomainFailedException` where the original exception's class implements `IFailure`, and `DomainFaultedException` itself for every other exception, cancellation included.")

rep("L15", LIB, "§9.1, *The radix alphabet*",
    "a value outside the type's range raising `ParseRangeException` (*The range failure*, below)",
    "a value outside the type's range raising `OverflowException`")

rep("L16", LIB, "§9.1, *The range failure* (removed with its declaration)",
    "\n\n**The range failure.** A `Parse` member — here, in [§9.2](#sec-double-and-float) and [§9.3](#sec-decimal), and through [§2.3](#sec-convert)'s `Convert.To*` — whose input is well-formed and spells a value the type cannot represent raises `ParseRangeException`:\n\n```bezel\npublic class ParseRangeException: OverflowException, IFailure\n  public pure constructor();\n  public pure constructor(message: string);\n  public constructor(message: string, cause: Exception);\nend ParseRangeException;\n```\n\nInput from outside the program is a condition a correct program can meet, so the class implements `IFailure` ([§1](#sec-failure-signal-rule)). It derives from `OverflowException` so that an `OverflowException` arm around a `Parse` call matches as it always has; the arithmetic `OverflowException` of language spec [§6.1](Bezel_Language_Spec.md#sec-integer-arithmetic) is a defect, and no arm on `IFailure` takes it. Wherever this document writes *the `FormatException`/`OverflowException` contract* of a `Parse` family, the second class raised is this one. It lives in the auto-imported `System`, beside its producers, and a user type implementing `IParsable<T>` ([§2.19](#sec-static-abstractions)) raises the same two classes.",
    "")

rep("L17", LIB, "§9.2, *Input grammar*",
    "not the `ParseRangeException` below",
    "not the `OverflowException` below")

rep("L18", LIB, "§9.2, *Parsing and range*",
    "raises `ParseRangeException` ([§9.1](#sec-integer-types)): the type cannot represent the number the input spells",
    "raises `OverflowException`: the type cannot represent the number the input spells")

rep("L19", LIB, "§9.3, the range sentence",
    "raises — raises `ParseRangeException` ([§9.1](#sec-integer-types)).",
    "raises — raises `OverflowException`.")

# ------------------------------------------------------------------ rationale
rep("R1", RAT, "§11.4, the closing sentence",
    "The line between the rule's first and fourth bands is visible in the exception's class ([§11.12](#sec-failures-and-defects)).",
    "Where a band-4 condition comes from the environment or a peer, the exception's class says so ([§11.12](#sec-failures-and-defects)); no class says what a handler does with it.")

rep("R2", RAT, "§11.12, the heading and the first paragraph",
    "### 11.12 Failures and defects: the kind is a marker interface on the class\n\n(Proposal 0211.) The failure-signal rule (§11.4) separates a caller's bug from a failure consumed at a boundary. The difference is a fact about the exception's class — it implements `IFailure` or it does not (spec §31.4) — so that a handler can name it: the boundary arm names the marker, where an arm on `Exception` takes both.",
    "### 11.12 The failure marker: origin on the class, disposition in the handler\n\n(Proposals 0211 and 0214.) A handler at a boundary often wants to take what the outside world did to the program — a missing file, a dropped connection, a closed channel, a deadline — and let everything else through. The marker makes that a fact about the exception's class: it implements `IFailure` or it does not (spec §31.4), and the boundary arm names the marker where an arm on `Exception` takes everything.\n\n**Origin, not disposition.** The class states one thing its raiser knows: whether the environment or a peer produced the condition. It does not state whether the condition is expected, and a class without the marker does not state that the program is wrong. A reading that named the unmarked side *defect* and the marked side \"a condition a correct program can meet\" was declined (proposal 0214), because a measured sample of 272 task–operation pairs (`docs/research/failure-kind-by-context/`, re-rated blind on 100 of them) refutes both halves of it. The environment and peer classes were a bug in 3 of 106 rows. The classes it called defects were a bug in only about half of theirs. And what a program does about a condition varies with the call far more than with the class: the best rule keyed on the type predicts 53% of the dispositions, who supplied the operand 70%. Text that does not parse and a document that does not deserialize sit between: whether the input was a user's or the program's own constant decides, and the library cannot see it (bugs in 8 of their 66 rows, most of the bugs raised with a marked class in that reading). So `FormatException` and `SerializationException` carry no marker, and a handler that expects bad input names them. The disposition — carry on, report, retry, stop — is always the handler's: catching a missing database file and stopping is a handler.")

rep("R3", RAT, "§11.12 *An interface, not a base class*, the first ground",
    "A class has one base, so a band drawn as a subtree cannot hold a class that must also stay under another: the out-of-range `Parse` input, which existing `OverflowException` arms take, and the fault a domain join raises, which existing `DomainFaultedException` arms take. Each would have had to leave its base, and the arms written against that base would have stopped matching.",
    "A class has one base, so a band drawn as a subtree cannot hold a class that must also stay under another: the fault a domain join raises for a failure, which existing `DomainFaultedException` arms take, and a user's class marked under a base of the documents (`class StaleOrder: InvalidOperationException, IFailure`), which the user's arms on that base take. Each would have had to leave its base, and the arms written against that base would have stopped matching.")

rep("R4", RAT, "§11.12 *An interface, not a base class*, the third ground",
    "Third, the precedent is against the subtree: Java drew its checked/unchecked line by base class and filed `NumberFormatException` — malformed input, the plainest failure there is — under the unchecked `IllegalArgumentException`, because its is-a parent was on the other side of the line. A kind that cuts across the is-a tree needs a mechanism that can.",
    "Third, the precedent is against the subtree: Java drew its checked/unchecked line by base class, and its own library added `UncheckedIOException` to carry I/O errors across it where a use needed them on the other side. A marker that cuts across the is-a tree needs a mechanism that can.")

rep("R5", RAT, "§11.12 *Why the marker has no members, and who may carry it*, the second restriction",
    "A second restriction keeps the documents' own classification true: a class under a built-in or standard-library class that is not a failure may not list the marker. Without it `class C: OperationCanceledException, IFailure` would make a failure arm take a cancellation, and a user's failure under `InvalidOperationException` would be taken by every state-defect arm — the swallowing stdlib §4.8 keeps `KeyNotFoundException` clear of. The library's two bridging classes are the only ones across that line, each recorded with its reason; a user hierarchy under a user base stays free to mix kinds, since no arm on a class of the documents is affected.",
    "A second restriction keeps cancellation apart: a class under `OperationCanceledException` may not list the marker. Cancellation answers a request the program itself made, and a failure arm — a retry loop, `IFailure => ()` — that took it would retry or swallow the request to stop. The broader restriction, no marker under any unmarked class of the documents, was declined (proposal 0214): an unmarked class makes no statement, so a user class `StaleOrder: InvalidOperationException, IFailure` contradicts nothing, and an arm on the base taking it is what the user's own derivation asked for. The swallowing stdlib §4.8 guards against is the library choosing a base for its own classes, which the library still controls.")

rep("R6", RAT, "§11.12 *Unmarked means defect*",
    "**Unmarked means defect.** A user exception class is a defect report until it lists the marker. The other default would let a boundary arm swallow whatever a library author forgot to classify, which is the failure this entry exists to remove; the cost of the chosen default is one interface name on each class meant to be handled.",
    "**Unmarked makes no statement.** A class that does not list the marker says nothing about where its condition came from. Reading it as a bug report was declined (proposal 0214): about half of the sample's rows raised with such a class were not bugs. Reading it as a failure was declined too (proposal 0211): an `IFailure` arm would then take whatever a library author forgot to classify, which is what the marker exists to keep out. The cost of the chosen default is one interface name on each class meant to be taken by an `IFailure` arm.")

rep("R7", RAT, "§11.12 *What the marker does not do*",
    "Abandoning the process or the domain on a defect was weighed and declined: it would skip the cleanup the exception edge guarantees, and it would make a contract violation uncatchable, where spec §22.16 makes it a catchable exception and the test oracle and the supervisor both depend on catching one. It leaves the query/assert tiers alone: a missing key and an empty collection stay defects in their throwing forms, because the program owns the state each tests (stdlib §1, *Failures and defects*); the throwing `Parse` raises a failure, because the text is the outside's. The kind is the condition's, not the tier's.",
    "Abandoning the process or the domain on a bug was weighed and declined: it would skip the cleanup the exception edge guarantees, and it would make a contract violation uncatchable, where spec §22.16 makes it a catchable exception and the test oracle and the supervisor both depend on catching one. It leaves the query/assert tiers alone: a throwing form raises its condition's class whatever the caller's stance — the missing key and the empty collection their unmarked classes, `Parse` its unmarked `FormatException`, `Console.ReadKey` its marked `EndOfStreamException` (stdlib §1, *The failure marker*). The marker is the condition's, not the tier's. And it does not convert: a caller for whom a failure at one call means a bug — a schema file shipped with the service — has no one-token form to say so. That conversion is deferred (proposal 0214) until programs written against the marker show it is needed.")

rep("R8", RAT, "§11.12 *Reopening bars*",
    "For the base class: evidence that the catch-arm binding rule misleads readers, together with a migration for the two bridging classes.",
    "For the base class: evidence that the catch-arm binding rule misleads readers, together with a migration for the domain join's bridging class and for user classes marked under a base of the documents. For marking a data class (`FormatException`, `SerializationException`) again: programs written against the marker whose boundaries answer bad input through an `IFailure` arm far more often than they name the class, with the bugs such an arm would then answer as routine counted.")

# ------------------------------------------------------------------ Guide 2
rep("G2-1", G2, "the pin",
    "re-validated against v0.356.*",
    "re-validated against v0.358.*")

rep("G2-2", G2, "the opening list",
    "- A **caller bug** is a contract (`requires`, §7) or a defect exception: a report, not something to handle.\n- A **failure** — I/O, network, bad input: anything a correct program can meet, logged or retried rather than branched on — is a typed exception whose class implements `IFailure` (§4).",
    "- A **caller bug** is a contract (`requires`, §7) or one of the residual exceptions (`ArgumentException`, `InvalidOperationException`): a report to the calling code.\n- A **failure consumed at a boundary** — logged, retried or reported rather than branched on — is a typed exception. When the environment or a peer produced it (I/O, a closed channel, a deadline), its class implements `IFailure` (§4).")

rep("G2-3", G2, "after the opening list",
    "exceptions are the boundary-and-defect tool, not the default.",
    "exceptions are the boundary tool, not the default.")

rep("G2-3b", G2, "§2, the first sentence",
    "Expected failure is a value, not a throw — exceptions are the boundary-and-defect tool (§4).",
    "Expected failure is a value, not a throw — exceptions are the boundary tool (§4).")

rep("G2-4", G2, "§4, the first sentence",
    "the failure consumed at a boundary, and the defect a `requires` contract (§7) cannot state",
    "the failure consumed at a boundary, and the bug a `requires` contract (§7) cannot state")

rep("G2-5", G2, "§4, *Failures and defects* through the class-of-your-own paragraph",
    "**Failures and defects.** Every exception is one of two kinds, and its class says which (spec §31.4). A *failure* is something a correct program can meet — a missing file, malformed input, a peer that went away, a deadline — and its class implements the marker interface `IFailure`. Everything else is a *defect* report: an index out of range, a broken contract, a null where none was promised. `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException` and the channel exceptions are failures; `ArgumentException`, `InvalidOperationException`, `IndexOutOfRangeException`, `OverflowException`, `ContractViolationException` and their relatives are defects. (`OperationCanceledException` is neither: it answers a cancellation you asked for.)\n\nThat gives a boundary its arm. Where you would have reached for `Exception`, name the marker:\n\n```bezel\nmethod Serve(request: Request): Response\n  try\n    return Dispatch(request);\n  catch\n    var f: IFailure => do                    // expected: answered\n      return Response.BadGateway(f.Message);\n    end,\n    var ex: Exception => do                  // a bug: reported, not hidden\n      ReportDefect(ex);\n      return Response.InternalError();\n    end\n  end;\nend Serve;\n```\n\nThe `IFailure` arm takes every failure and lets every bug through; `f` is an ordinary `Exception` inside the arm. The second arm is there only because this boundary is a supervisor that must keep serving. Most boundaries have no second arm: a bug then propagates, and an uncaught one ends the program with its report (spec §33.3). Where cancellation can reach a supervisor, give it its own arm ahead of the `Exception` arm — `OperationCanceledException => throw` — so that it is not reported as a bug. One arm on bare `Exception` that handles both is the habit to drop: it answers a bug as if it were bad input. An exception class of your own is a defect until you say otherwise:\n\n```bezel\npublic class ConfigError(message: string): Exception(message), IFailure\nend ConfigError;\n```\n\nA class may list `IFailure` only if nothing above it but `Exception` is a built-in or standard-library class (spec §31.4). So a failure class that derives from `InvalidOperationException` or `ArgumentException` — a habit carried over from C# — has to be re-based before it can be marked: `class OrderError(message: string): Exception(message), IFailure`. Arms that named the old base (`InvalidOperationException => …`) no longer take it, so find those arms first; an arm on the class itself, or on `IFailure`, is unaffected. A class that derives from a failure type (`IOException`, `FormatException`) is a failure already and needs no change.\n\nTwo habits keep the kinds honest. Wrap a failure in a failure and never wrap a defect: re-raise it. And when out-of-range text matters, catch `FormatException` and `ParseRangeException` (or just `IFailure`); `ParseRangeException` is the failure `int.Parse(\"99999999999\")` raises, distinct from the arithmetic `OverflowException` of `a * b`, which is a defect.",
    "**The failure marker.** Some exceptions say where they came from. A class that implements the marker interface `IFailure` is a *failure*: the outside world disagreed — a missing file, a refused permission, a peer that closed its channel, a deadline. The `IOException` family, `TimeoutException`, `ChannelClosedException`, `ChannelFullException` and `DomainFailedException` are failures (spec §31.4, stdlib §1). Every other class makes no statement: a `FormatException` may be a user's typo or a bug in your own constant, a `KeyNotFoundException` a bad key from a request or a slip in your own bookkeeping. (`OperationCanceledException` is apart: it answers a cancellation you asked for.)\n\nKeep three things apart: *the class says who could have caused it; the call says whether you expected it; the handler says what happens next.* The marker is the first, and only the first. A missing file is a failure whether the user picked the file or your program cannot start without it; what you do about it is your handler's choice.\n\nThat gives a boundary its arms. Where you would have reached for `Exception`, name what this boundary expects — the classes it can answer, and the marker for what the environment and its peers do:\n\n```bezel\nmethod Serve(request: Request): Response\n  try\n    return Dispatch(request);\n  catch\n    var e: FormatException => do             // bad client input: expected here\n      return Response.BadRequest(e.Message);\n    end,\n    var f: IFailure => do                    // the environment or an upstream peer\n      return Response.BadGateway(f.Message);\n    end,\n    var ex: Exception => do                  // everything else: reported, not hidden\n      ReportBug(ex);\n      return Response.InternalError();\n    end\n  end;\nend Serve;\n```\n\nThe `FormatException` arm is this service's statement that malformed input from a client is expected here; nothing in the class says so, because the same class reports a malformed constant in your own code. The `IFailure` arm takes what the environment or an upstream peer did; `f` is an ordinary `Exception` inside the arm. The last arm is there only because this boundary is a supervisor that must keep serving. Most boundaries have no last arm: anything else then propagates, and an uncaught exception ends the program with its report (spec §33.3). Where cancellation can reach a supervisor, give it its own arm ahead of the `Exception` arm — `OperationCanceledException => throw` — so that it is not reported as a bug. One arm on bare `Exception` that answers everything the same way is the habit to drop: it answers a bug as if it were bad input.\n\nThe marker is a default, not a verdict. If the service cannot run without a file, an `IFailure` arm above a lazy open of it answers its absence as routine. Open such a file at start-up, where no arm is above it, or catch `FileNotFoundException` there and stop.\n\nAn exception class of your own makes no statement until you say otherwise:\n\n```bezel\npublic class ConfigError(message: string): Exception(message), IFailure\nend ConfigError;\n```\n\nA class may list `IFailure` under any base except `OperationCanceledException` (spec §31.4). A hierarchy carried over from C# keeps its base: `class StaleOrder(message: string): InvalidOperationException(message), IFailure` is taken both by an `IFailure` arm and by an arm on `InvalidOperationException`. A class that derives from a failure type (`IOException`) is a failure already and needs no listing. Mark a class when its condition is the outside world's, or when your application wants its boundary's `IFailure` arm to take it — a `ConfigError` for a configuration file the operator wrote, say.\n\nTwo habits keep the marker honest. When you wrap, the wrapper's class is what the handler above will read: wrap a failure in a failure, and re-raise an exception you cannot vouch for rather than wrapping it in a class that lists `IFailure`. And where bad text is expected, name what `int.Parse` raises — `FormatException` for a malformed numeral, `OverflowException` for an out-of-range one — or, better, take `ParseOrNull` and decide at the call (§2).")

rep("G2-6", G2, "§5, the `Handle` example's comment",
    "    var ex: IFailure => RecordError(ex)   // ex is an Exception here; a bug propagates",
    "    var ex: IFailure => RecordError(ex)   // ex is an Exception here; anything else propagates")

rep("G2-7", G2, "§7, the boundary-consumed row",
    "| a typed exception whose class implements `IFailure`, producer-local (spec §31.4); catch it at the boundary with an `IFailure` arm and copy out what you keep (§4, §5) |",
    "| a typed exception, producer-local (spec §31.4), listing `IFailure` when the environment or a peer produced it; catch it at the boundary — by name, or with an `IFailure` arm — and copy out what you keep (§4, §5) |")

rep("G2-8", G2, "§7, the boundary row",
    "| Handling whatever goes wrong at a boundary | an `IFailure` arm; a separate `Exception` arm only in a supervisor that reports the bug, never one arm for both (§4) |",
    "| Handling what goes wrong at a boundary | an arm for each class this boundary expects (`FormatException` for bad client input) and an `IFailure` arm for what the environment and peers do; a separate `Exception` arm only in a supervisor that reports the rest, never one arm for everything (§4) |")

rep("G2-9", G2, "§7, the closing paragraph",
    "And at the boundary the arm names `IFailure`: an arm on bare `Exception` takes the bugs too.",
    "And at the boundary an arm names what the boundary expects — the classes it can answer, and `IFailure` for the outside world's — where an arm on bare `Exception` takes the bugs too.")

# ------------------------------------------------------------------ Guide 4
rep("G4-1", G4, "§5, the join",
    "Its kind does cross: a failure in the child is joined as `DomainFailedException`, a `DomainFaultedException` that an `IFailure` arm also takes, and a bug in the child is joined as the plain class, which that arm lets pass (Guide 2 §4).",
    "The failure marker does cross: a failure in the child is joined as `DomainFailedException`, a `DomainFaultedException` that an `IFailure` arm also takes, and any other fault in the child is joined as the plain class, which that arm lets pass (Guide 2 §4).")

rep("G4-2", G4, "§8, the mutable-static bullet (re-trace: §22.12 form (iv) was retired at v0.334)",
    "- **A mutable static** is a compile error unless it is one of four admitted forms:",
    "- **A mutable static** is a compile error unless it is one of three admitted forms:")

rep("G4-3", G4, "§8, the same bullet, form (iii) and the retired form (iv)",
    "admitted *with a warning* and no cross-domain promises; (iv) the owning-inline-array slot pool, likewise warned.",
    "admitted *with a warning* and no cross-domain promises. (An owning-inline-array slot pool is a `static readonly` field instead: warned, its slots fixed, disposed at exit.)")

# ------------------------------------------------------------------ Guide 6
rep("G6-1", G6, "§2, the first paragraph",
    "with defect exceptions like `ArgumentException` as the pre-contract residual",
    "with residual exceptions like `ArgumentException` at a pre-contract boundary")

rep("G6-2", G6, "§2, the supervision-boundary paragraph",
    "the sanctioned signal is a typed exception whose class implements `IFailure`: a failure (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).",
    "the sanctioned signal is a typed exception (Guide 2 §4; the failure-signal rule's band 4, stdlib §1).")

rep("G6-3", G6, "§2, the `Listen` rewrite's comment",
    "    // a requires. FormatException is the built-in input failure: its class\n    // implements IFailure (spec §31.4).",
    "    // a requires. FormatException is the built-in format fault (spec §31.4).")

rep("G6-4", G6, "§2, why not the throwing `int.Parse`",
    "You may. What it raises is a failure either way — `FormatException` for a malformed numeral, `ParseRangeException` for an out-of-range one (stdlib §9.1, §2.3) — because the text came from outside the program, and a boundary's `IFailure` arm takes both (Guide 2 §4). The explicit throw is for the wording: `int.Parse` reports two conditions in the numeral's terms, while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words. The kind is also what rules out `ArgumentException`: the `Argument*` family is part of band 1's pre-contract defect residual (stdlib §1) — the exception that blames the *calling code* — and no caller is at fault here. Throwing it would plant a defect report in the boundary's log — telling whoever triages that log that the calling code needs fixing, when it does not; `FormatException` blames the input's shape, which is what actually failed.",
    "You may, and the boundary sees an exception either way. But `int.Parse` splits the condition: it raises `FormatException` for a malformed numeral and `OverflowException` for an out-of-range one (stdlib §9.1, §2.3), while `Listen`'s condition is single — *not a port*. The explicit throw keeps one condition on one signal, in the domain's own words, so a boundary that expects bad ports names one class. What rules out `ArgumentException` is what it says: the `Argument*` family is band 1's pre-contract residual (stdlib §1) — the exception that blames the *calling code* — and no caller is at fault here. Throwing it would plant a report in the boundary's log telling whoever triages it that the calling code needs fixing, when it does not; `FormatException` blames the input's shape, which is what actually failed. Neither class implements `IFailure`: whether bad text is expected depends on who supplied it, which only the boundary knows (spec §31.4), so the boundary that expects it names `FormatException`.")

rep("G6-5", G6, "§2, the two-question test",
    "The kinds follow the answer: a broken `requires` raises `ContractViolationException`, a defect report that a boundary's `IFailure` arm lets pass, and the typed exception for bad input is a failure, which that arm takes (spec §31.4).",
    "The exceptions follow the answer: a broken `requires` raises `ContractViolationException`, which reports the calling code, and the typed exception for bad input blames the input; the boundary names the one it expects (Guide 2 §4).")

# ------------------------------------------------------------------ Tour
rep("T1", TOUR, "§6, exceptions",
    "**Exceptions** are for what the caller does not branch on, and are of two kinds: a *failure* (a missing file, bad input), whose class implements `IFailure`, and a *defect* report, which is a bug. `IFailure => …` is the arm that takes every failure and no bug.",
    "**Exceptions** are for what the caller does not branch on. A class that implements `IFailure` is a *failure*: the outside world disagreed (a missing file, a closed channel), and `IFailure => …` is the arm that takes all of them. What to do about one is the handler's call.")

# ------------------------------------------------------------------ code-generation reference
rep("C1", CG, "§5.1, the three access tiers",
    "(the call asserts the value is there and raises otherwise: a *failure* where the condition is the outside's — `Parse`, `Console.ReadKey` — and a *defect* where it is the program's own state — `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`)",
    "(the call asserts the value is there and raises otherwise — `Parse`, `Console.ReadKey`, `dict[key]`, `First()`, `PopAt`, `Pop`, `Dequeue`; of these only `Console.ReadKey`'s `EndOfStreamException` implements `IFailure`)")

rep("C2", CG, "§5.2, the `Convert` comment",
    "end                                                    // failures: FormatException, ParseRangeException",
    "end                                                    // raises FormatException, OverflowException")

rep("C3", CG, "§5.2, the exception hierarchy",
    "**Kinds (§31.4):** a class that implements the member-less interface `IFailure` is a *failure* — `FormatException`, the `IOException` family, `SerializationException`, `TimeoutException`, `ChannelClosedException`, `ChannelFullException`, `ParseRangeException` (an `OverflowException`: out-of-range `Parse`/`Convert.To*` input) and `DomainFailedException` (a `DomainFaultedException`: a failure joined from another domain); every other is a *defect* report; `OperationCanceledException` is neither. Only an `Exception`-derived class may list `IFailure`, and not one under a built-in or stdlib class that is not itself a failure.",
    "**The failure marker (§31.4):** a class that implements the member-less interface `IFailure` is a *failure* — the environment or a peer produced it: the `IOException` family, `TimeoutException`, `ChannelClosedException`, `ChannelFullException` and `DomainFailedException` (a `DomainFaultedException`: a failure joined from another domain). No built-in class is one, and every other class makes no statement (`FormatException`, `SerializationException` and `OverflowException` included); `OperationCanceledException` is apart. Only an `Exception`-derived class may list `IFailure`, and none under `OperationCanceledException`.")

rep("C4", CG, "§5.9, `double.Parse` out of range",
    "it rounds to nearest-even like a literal and raises `ParseRangeException` exactly where",
    "it rounds to nearest-even like a literal and raises `OverflowException` exactly where")

rep("C4b", CG, "§5.9, the integer input grammar",
    "out of range `ParseRangeException` (an `OverflowException`; a failure)",
    "out of range `OverflowException`")

rep("C5", CG, "§6, declaring an exception type",
    "list `IFailure` when the condition comes from outside the program, since an unmarked class is a defect report (§31.4) — only where nothing in the base chain but `Exception` is a built-in or stdlib class, so a failure class derives from `Exception` or from a user class deriving from it (or from a failure type such as `IOException`, which needs no listing);",
    "list `IFailure` when the environment or a peer produces the condition, or when the application's boundary should take it as one — under any base but `OperationCanceledException`, and a class deriving from a failure type such as `IOException` needs no listing; an unmarked class makes no statement (§31.4);")

rep("C6", CG, "§6, catching at a boundary",
    "**Catching at a boundary.** Name the marker, not `Exception`: an `IFailure` arm takes every failure and lets every bug through, and its binding is an ordinary `Exception` (§31.2.1).\n\n```bezel\ntry\n  Serve(request);\ncatch\n  var f: IFailure => Respond(502, f.Message),                  // failures: answered\n  var ex: Exception => do ReportDefect(ex); Respond(500); end  // a supervisor only: bugs reported\nend;\n```\n\nLeave the second arm out unless the code is a supervisor: a bug should propagate. `IFailure => ()` suppresses failures only.",
    "**Catching at a boundary.** Name what the boundary expects, not `Exception`: the classes it can answer by name, and `IFailure` for what the environment and peers raise; an `IFailure` binding is an ordinary `Exception` (§31.2.1).\n\n```bezel\ntry\n  Serve(request);\ncatch\n  var e: FormatException => Respond(400, e.Message),           // bad client input, expected here\n  var f: IFailure => Respond(502, f.Message),                  // the environment or an upstream peer\n  var ex: Exception => do ReportBug(ex); Respond(500); end     // a supervisor only: the rest reported\nend;\n```\n\nLeave the last arm out unless the code is a supervisor: a bug should propagate. `IFailure => ()` suppresses failures only; `FormatException` and `SerializationException` are not failures, so a boundary that expects bad input names them.")

# pins
for i, old, new in [
    ("P1", "**Version 0.357 — November 2026**", "**Version 0.358 — November 2026**"),
]:
    rep(i, SPEC, "the version header", old, new)
for i, doc in [("P2", LIB), ("P3", RAT), ("P4", "docs/Bezel_Changelog.md")]:
    rep(i, doc, "the companion header",
        "**Companion to the Bezel Language Specification, v0.357 — November 2026**",
        "**Companion to the Bezel Language Specification, v0.358 — November 2026**")
rep("P5", CG, "the pin", "*Validated against v0.357.", "*Validated against v0.358.")
rep("P6", CG, "the citation pin",
    "are to spec v0.357 and `Bezel_Standard_Library.md` v0.357",
    "are to spec v0.358 and `Bezel_Standard_Library.md` v0.358")
rep("P7", CG, "the stdlib transcription pin",
    "transcribed from `Bezel_Standard_Library.md` v0.357",
    "transcribed from `Bezel_Standard_Library.md` v0.358")
rep("P8", G6, "the pin", "re-validated against v0.357.*", "re-validated against v0.358.*")
rep("P9", TOUR, "the pin", "*Validated against v0.357.", "*Validated against v0.358.")
rep("P10", G4, "the pin", "re-validated against v0.331.*", "re-validated against v0.358.*")


def load(root):
    docs = sorted({e["doc"] for e in E})
    return {d: open(os.path.join(root, d), encoding="utf-8").read() for d in docs}


def apply_all(texts):
    bad = []
    for e in E:
        t = texts[e["doc"]]
        n = t.count(e["old"])
        if n != 1:
            bad.append("%s (%s, %s): old occurs %d times" % (e["id"], e["doc"], e["where"], n))
            continue
        texts[e["doc"]] = t.replace(e["old"], e["new"], 1)
    return bad


def render():
    out = []
    for e in E:
        out.append("**%s** — `%s`, %s.\n" % (e["id"], os.path.basename(e["doc"]), e["where"]))
        out.append("Old:\n\n````text\n%s\n````\n" % e["old"])
        out.append(("New:\n\n````text\n%s\n````\n" % e["new"]) if e["new"] else "New: *(deleted)*\n")
    return "\n".join(out)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "render":
        print(render())
        sys.exit(0)
    root = sys.argv[2] if len(sys.argv) > 2 else "."
    texts = load(root)
    if cmd == "landed":
        miss = [e["id"] for e in E if e["new"] and e["new"] not in texts[e["doc"]]]
        print("landed: %d edits, %d missing %s" % (len(E), len(miss), " ".join(miss)))
        sys.exit(1 if miss else 0)
    bad = apply_all(texts)
    for b in bad:
        print("FAIL " + b)
    print("%s: %d edits over %d documents, %d anchor failures"
          % (cmd, len(E), len(texts), len(bad)))
    if bad:
        sys.exit(1)
    if cmd == "apply":
        for d, t in texts.items():
            with open(os.path.join(root, d), "w", encoding="utf-8") as f:
                f.write(t)
