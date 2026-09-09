# Bezel Guide 7: Style

*A short, example-first guide. Companion to the Bezel Language Specification — re-validated against v0.295.*

Nothing in this guide is enforced. The compiler checks none of the naming conventions — spec §1.3 states them and says in the same breath that it does not enforce them — no formatter ships with the toolchain, and every layout rule below compiles identically when it is ignored. What the conventions buy is that a reader recognizes a declaration's shape before reading its words, and that a diff shows the change rather than the reflow. Where a rule of style touches a rule of the language — the closers of §1.6, `_` as the reserved discard (§17.13), the no-hiding rule (§12.2), the field order that §28.4's reverse-order disposal makes load-bearing (§4) — the language rule is cited and never bent. The worked reference is the tour in `docs/proposals/0002-http-server.md` §2; the sections below generalize from it, and a formatter, when one exists, should encode them.

**Contents:** §1 naming · §2 layout · §3 comments · §4 order within a type · §5 files and modules · §6 a worked example · §7 what this guide does not decide.

---

## 1. Naming

| What | Convention | Example |
|---|---|---|
| Module | dotted PascalCase | `module Ledger.Accounts;` |
| Class, record, enum, choice | PascalCase | `Ledger`, `Posting`, `EntryKind`, `PostOutcome` |
| Interface | PascalCase with an `I` prefix | `IPostingRule` — as the stdlib does: `IDisposable`, `IEnumerable<T>`, `IParsable<T>` |
| Method, property, class-level `const`, enum constant, choice case, record positional field | PascalCase | `Post`, `Balance`, `MaxRecent`, `Debit`, `Posted(…)`, `record Posting(Account: string, …)` |
| Private field | `_camelCase` | `_balances` (spec §1.3) |
| Parameter, local, choice payload slot | camelCase | `amount`, `openingBalance`, `Rejected(reason: string)` |
| Type parameter | a single capital, or a PascalCase word where a letter would not read | `T`, `K`, `V`, `TElement` |
| Loop label | camelCase | `scan: for var row in rows do … end scan;` |

```bezel
public record Posting(Account: string, Amount: decimal);   // type and fields: PascalCase

public choice PostOutcome
  Posted(balance: decimal),                  // case names PascalCase, payload slots camelCase
  UnknownAccount,
  Rejected(reason: string);
end PostOutcome;

public interface IPostingRule                // the `I` is the stdlib's own convention
  method Admits(posting: Posting): bool;     // members PascalCase, parameters camelCase
end IPostingRule;
```

**Constants split by kind.** A class-level `const` and a compile-time `const` local are PascalCase — `const MaxUsers := 1000;` is spec §21.1's own example. A **runtime** `const` local (§21.1) is an ordinary local that happens not to be reassignable, and takes camelCase: `const openingBalance := account.Balance;`, matching §21.1's `const n := items.Count + 1;`.

**The three access tiers have three names** (stdlib §1). One lookup, three spellings, and the name is what tells them apart: the **plain** name throws and so treats absence as a bug (`Parse`, the `[key]` indexer); **`XOrNull`** returns `T?` for direct `??`/`?.` composition (`ParseOrNull`, `GetOrNull`); **`TryX`** is the only shape that distinguishes outcomes a collapsed `T?` cannot, in one of the four sanctioned forms (stdlib §1's table). Invent no fourth spelling — `GetSafe`, `MaybeGet`, `Get2` say nothing the tier names do not. Domain methods borrow the same vocabulary: the tour's store is `Find`/`Add`/`Update`/`Remove`, verbs a reader already knows from the collections.

**`Async` marks a pair, not a signature.** The rule the stdlib actually follows (stdlib §1, v0.195) is narrower than the habit: the suffix marks the asynchronous half of a **sync/async pair** — `Acquire`/`AcquireAsync`, `Send`/`SendAsync`, `ReadAll`/`ReadAllAsync`, `Lock`/`LockAsync`. An operation that exists **only** asynchronously carries no suffix at all: `Channel<T>.Receive`, `TaskScope.Join`, `Task.Delay`. `Receive` beside `SendAsync` on one endpoint pair is the rule working, not an inconsistency. So an `async method Post` with no synchronous sibling is named `Post`, and adding a synchronous sibling later is what renames it.

**Booleans read as predicates.** `IsEmpty`, `HasEntries`, `CanPost`, `Done` — or a verb the caller can read as a question: `Contains`, `TryAdd`, `Admits`. Never a noun that reads as data (`Status`, `Flag`).

**Lambda parameters are spelled out** unless the lambda is a one-token projection:

```bezel
using var amounts := postings.Select(lambda(p) => p.Amount);      // a projection: a letter is fine
using var settled := postings.Where(                              // anything more than a projection:
    lambda(posting) => Settles(posting, today));                  //   spell the parameter out
```

**`_` is only ever the discard** (spec §1.3, §17.13): the statement `_ := ComputeChecksum();`, a lambda slot the body ignores (`lambda(_) => body`), a pattern slot that binds nothing. It is never a name — `var _ := e;`, `out var _`, a parameter or field named `_` are compile errors, each with a diagnostic naming the sanctioned form. An identifier that merely *begins* with an underscore (`_balances`) is an ordinary identifier, and that is what makes `var balance := _balance;` unambiguous in a body.

**A name is never reused down an inheritance chain**, and here the language agrees: §12.2 forbids hiding outright, so a derived member whose name a visible inherited member already has is a compile error unless it is an `override`. The style rule is only ever "pick the different name early".

**Test names state the behavior asserted.** `CreateReturnsTheStoredTodo`, `GetByIdRejectsANonNumericId`, `PostRejectsAnUnknownAccount` — subject, verb, object, in ordinary member PascalCase. Never a status code (`Test404`), never "works" (`CreateWorks`), never a number (`PostTest3`). A test whose name cannot be written this way is usually asserting more than one thing.

---

## 2. Layout

**Two spaces per level, no tabs.** A member body is one level in from its declaration, a nested block one more.

**Close a declaration with its name.** `end Ledger;`, `end Post;`, `end constructor;`. Spec §1.6 admits a bare `end;` everywhere and §11.4 recommends the named form; this guide requires it on every *declaration* — class, interface, record, enum, choice, method, block property, constructor, destructor, operator — because after a long body the name is the reader's anchor, and the compiler verifies it matches. Inside a body, close statement blocks with the bare `end;`: `if`, `for`, `while`, `do`, `try`, `case`. Two closers are not a choice: an indexer block closes `end;` (an indexer has no name), and a **labeled** loop closes `end scan;` — the keyword closer is rejected once the loop is labeled (§1.6).

**One statement per line, and every `if` in block form** — the condition on its own line, the body indented, `end;` on the last, as §21.2's own examples are written. Never `if account = null then return false; end;`. The grammar admits it — every statement ends in `;`, so a block can be typed on one line — but a one-line conditional hides its own branch from a scan and from a diff. Brevity has its own form: the §20 `if` **expression**, `(if a > b then a else b)`, whose `else` is mandatory and which is an expression, not a squeezed statement.

**A blank line between the logical steps of a body.** Acquire, compute, store, return; in a test, arrange, act, assert. A two-line body gets none.

```bezel
public static method PostRejectsAnUnknownAccount()
  using var ledger := new Ledger(["ada"]);                     // arrange

  var posted := ledger.Post(new Posting("bob", 25m));          // act

  Debug.Assert(not posted, "an unknown account is declined");  // assert
end PostRejectsAnUnknownAccount;
```

Blank lines separate members too, and separate a clause region from the body it guards — spec §22.16's own recommended style: the promises above the gap, the doing below it.

**Continuations are indented past the statement they continue** — four columns (two levels), or aligned to the opening parenthesis when a parameter list wraps. One level is the column a nested block's body occupies, which is exactly the ambiguity the indent is there to remove.

```bezel
_accounts.Add(posting.Account,
    new Account(posting.Account));           // four columns past the statement's own indent

public constructor(owners: Array<string>,
                   recentLimit: int)                   // wrapped parameter list: paren-aligned
  _limit := recentLimit;
end constructor;
```

**Keep code within 110 columns.** The tour's longest code line is 103. A trailing-comment block picks one column for the whole block and may carry a line past the ceiling — the tour's comments start at column 78 and its longest line is 135. Align the comments; do not reflow working code to hide them.

**Align a run, never a body.** Where consecutive lines have the same shape — a table of registrations, a run of `case` arms, a wrapped named-argument list — align the columns, so a wrong entry shows up as the ragged one:

```bezel
public method Describe(outcome: PostOutcome): string =>
    (case outcome of
      Posted(var balance) => $"posted, balance {balance}",
      UnknownAccount      => "no such account",
      Rejected(var why)   => $"rejected: {why}");
```

Do not align `:=` across ordinary, unrelated statements: the alignment is not information, and the next edit reflows every line in the group into the diff.

**Expression bodies for one-expression members, block bodies otherwise.** `public property Balance: decimal => _balance;` and `public method Credit(amount: decimal) => _balance := _balance + amount;` are the right shape; an `=>` whose expression needs more than one continuation line is a block body written sideways.

---

## 3. Comments

**Trailing comments say what the reader cannot see.** The ownership fact, the spec section, the status a shape answers, the reason a line is where it is — not a paraphrase of the statement. `// increment the count` above `count++;` costs a line and earns nothing.

```bezel
var account := _accounts.GetOrNull(posting.Account);   // a borrow, not a take (stdlib §4.9)
if account = null then
  return false;                                        // an outcome, not a fault (stdlib §1 band 2)
end;
```

**One comment column per block.** Pick a column past the longest code line in the run and hold it for that block; a continuation of a trailing comment indents under its own text, as the stdlib's own listings do. When the next block's code is wider, that block picks its own column — the alignment is local, and a single file-wide column would drag every comment right.

**Cite the spec where the code derives from a rule** — `// per §28.3` in code, `(§28.3)` inside a sentence. Give the revision when the citation is load-bearing outside the file (a PR, a test name, an issue).

**Banner comments only between top-level declarations.** `// ── Wire shapes ──────────` separates the sections of a file; inside a body it is decoration, and a body long enough to need one wants a private method instead.

---

## 4. Order within a type

Class-level constants and fields, constructors, the destructor if there is one, properties, public methods, private methods. A reader meets the state, then the ways it is built, then the surface, then the machinery.

**One rule here is not taste.** §28.4's generated `Dispose()` disposes owned-bearing fields in **reverse declaration order**, so a field declared later is torn down while an earlier one is still intact. Read forward: **a later field may borrow from an earlier one, never the reverse** — and the field that owns the borrowers is declared **last**.

```bezel
public sealed class Ledger
  private owned _accounts: OwningDictionary<string, Account> := new();   // borrowed FROM
  private owned _recent: Array<Account> := new();                        // holds borrows — LAST (§28.4)
end Ledger;
```

The worked case is `0002-http-server.md` §4.1: the root service declares `_deps` — whose store the mounted children borrow — and then `_routes`, which *owns* those children. `_routes` is disposed first, the children die while `_deps` is intact, and §28.3's back-reference residual ("the source outlives the object") is discharged **structurally**, by declaration order, with no runtime backstop. Swap the two lines and the borrow outlives its source — a residual §28.3 places on the field order, reported at disposal if at all, never at either declaration. Where the ordering carries this weight, say so in a trailing comment — the reader cannot see it, which is §3's whole test.

---

## 5. Files and modules

**One `module` per file** (spec §2.1: it is the first non-comment line, and there is exactly one), named in dotted PascalCase. Name the file for its principal declaration — `Ledger.bzl` — and the module for the area it belongs to.

**Then the `import` block**, one per line, no blank lines inside it, with a trailing comment naming what the import is for when the name does not say. Only a non-auto module needs a line at all (spec §2.3).

```bezel
module Ledger.Accounts;
import System.Threading;            // Locked<T> (stdlib §6.11)
import System.Diagnostics;          // Debug.Assert (stdlib §8.2)
```

**Then declarations in dependency order**: the value shapes first (records, enums, choices, the interfaces they satisfy), then the stores and services built over them, then the `Program` class last. A reader meets each name after its definition, and the file reads top to bottom exactly once.

**Tests live in a `<Module>.Tests` module** that imports the module under test — `module Ledger.Accounts.Tests;` — one test class per type under test, and test methods named as §1 says.

---

## 6. A worked example

Given a `Posting` record and an `Account` class:

```bezel
public record Posting(Account: string, Amount: decimal);

public class Account
  private readonly _owner: string;
  private _balance: decimal := 0m;

  public constructor(owner: string)
    _owner := owner;
  end constructor;

  public property Owner: string => _owner;
  public property Balance: decimal => _balance;

  public method Credit(amount: decimal) => _balance := _balance + amount;
end Account;
```

Here is a `Ledger` that breaks six numbered rules below — plus §3's, since its one comment restates its own line. Every line of it compiles:

```bezel
public class ledger                                          // 1 — a type is PascalCase
  private owned _recent: Array<Account> := new();            // 2 — the borrower is declared FIRST (§28.4)
  private owned _accounts: OwningDictionary<string, Account> := new();
  public const max_recent: int := 8;                         // 3 — a const is PascalCase
  public constructor(Owners: Array<string>)                  // 4 — a parameter is camelCase
    for var o in Owners do _accounts.Add(o, new Account(o)); end;
  end constructor;
  public method post(P: Posting): bool                       // 5 — one-line blocks, no blank lines
    var a := _accounts.GetOrNull(P.Account);
    if a = null then return false; end;
    a.Credit(P.Amount);                                      // add the amount to the account  ← §3
    _recent.Add(a);
    return true;
  end;                                                       // 6 — a declaration closes with its name
end;
```

The same class, written to this guide. Three differences change what the program *does* — the field order (§4), the `requires` clause, and `sealed`; every other one changes only what a reader has to reconstruct:

```bezel
public sealed class Ledger
  public const MaxRecent: int := 8;
  private owned _accounts: OwningDictionary<string, Account> := new();   // borrowed FROM
  private owned _recent: Array<Account> := new();                        // holds borrows — LAST (§28.4)

  public constructor(owners: Array<string>)
    for var owner in owners do
      _accounts.Add(owner, new Account(owner));
    end;
  end constructor;

  public property Count: int => _accounts.Count;

  public method Post(posting: Posting): bool
    requires(posting.Amount > 0m)

    var account := _accounts.GetOrNull(posting.Account);   // a borrow, not a take (stdlib §4.9)
    if account = null then
      return false;                                        // an outcome, not a fault (stdlib §1 band 2)
    end;

    account.Credit(posting.Amount);                        // narrowed by the early return (§7.4)
    Remember(account);

    return true;
  end Post;

  private method Remember(account: Account)
    if _recent.Count = MaxRecent then
      _recent.RemoveAt(0);
    end;

    _recent.Add(account);                                  // a borrow into _accounts — hence the field order
  end Remember;
end Ledger;
```

---

## 7. What this guide does not decide

A **formatter**: none exists, and until one does, the rules above are applied by hand and reviewed like any other part of a change; when one lands, this document is its specification and its output settles the cases this text leaves open. **Display**: the file contains two-space indents and no tabs, and how an editor renders them is not a project matter. And **anything the specification decides** — which closers are admitted (§1.6), modifier order (§11.5), where `_` may appear (§17.13), visibility (§13), the disposal order §4 leans on (§28.4) — is a rule, not a convention, and this guide only cites it. Where this guide and the spec appear to disagree, the spec wins and the disagreement is a defect worth reporting.
