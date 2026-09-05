#pragma once

// stdlib §5 LINQ operator overload table.
//
// fix #3726 (stdlib §5 `System.Collections.Linq`, v0.286) — exported from
// `ExprTypecheck.cpp` so `ExprTypecheck_Calls.cpp`'s `typeLinqOperator` can
// gate its name-only pass-through match on `(name, ce.args.size())`. The
// pre-fix lambda typed a `Where(1, 2)` / `Take(1, 2)` / `Where()` call as
// `IEnumerable<T>` unconditionally (the §5 name exemption in
// `kBuiltinCollectionLinqOperatorNames` is name-only, not arity-aware), so
// codegen's #2434/#2491 zero-init degradation notes let the call compile
// rc=0, link, and run with `count=0` — a silent zero-degrade on a LIVE
// stdlib §5 operator.
//
// The table itself predates this fix: it was `static` inside
// `ExprTypecheck.cpp` (consulted only by the §26.5 extension-ambiguity
// path, `lookupExtensionMember`). The fix hoists it into a header so the
// lazy-typing arms in `ExprTypecheck_Calls.cpp` can consult the SAME
// source of truth (a comment on the table already noted it "mirrors stdlib
// §5's listing" — that promise now reaches the two readers it was always
// meant for).
//
// The shape stays the same: one row per stdlib §5 overload (name + arity +
// `where` clause). The header is `inline constexpr` so the table has one
// definition across translation units (the existing `static` would collide
// with a non-static declaration here, and the table is small enough that
// the per-TU bloat of an `inline` storage is irrelevant).

#include <cstddef>
#include <string_view>

namespace bezel::sema {

struct StdlibLinqOverload {
    const char* name;
    int arity;  // one entry per overload of `name` — name + arity match a
                // specific stdlib §5 method.
    const char* where;  // "" for the main `extension IEnumerable<T>` block;
                        // "where T: hashable" / "where T: IComparable<T>" /
                        // "where T: INumber<T>" for the constrained blocks.
                        // The §26.5 rule 4 diagnostic's "declared head
                        // including any where clause" wording (§26.5.1
                        // v0.278) applies uniformly to stdlib and user
                        // candidates — the where clause is part of the
                        // declared head, not a separate fact.
};

inline constexpr StdlibLinqOverload kStdlibLinqOverloads[] = {
    // Main `extension IEnumerable<T>` block (stdlib §5 line 3497)
    // Filtering
    // fix #3559: `OfType` was removed from stdlib §5 at v0.284, so it is no
    // longer a row of the table this list mirrors.
    {"Where", 1, ""},
    // Projection
    {"Select", 1, ""}, {"SelectMany", 1, ""},
    // Quantifiers
    {"Any", 0, ""}, {"Any", 1, ""}, {"All", 1, ""},
    {"Count", 0, ""}, {"Count", 1, ""},
    // Element access
    {"First", 0, ""}, {"First", 1, ""},
    {"FirstOrNull", 0, ""}, {"FirstOrNull", 1, ""},
    {"Last", 0, ""}, {"Last", 1, ""},
    {"LastOrNull", 0, ""}, {"LastOrNull", 1, ""},
    {"Single", 0, ""}, {"Single", 1, ""},
    {"SingleOrNull", 0, ""}, {"SingleOrNull", 1, ""},
    {"ElementAt", 1, ""}, {"ElementAtOrNull", 1, ""},
    // Aggregation — selector forms (selector overloads live in the main
    // block; no-selector overloads live in §5.1's constrained blocks and
    // would each need a different `where` string; the ambiguity admission
    // is per overload, and no-selector overloads match an arity the user
    // could not have written for the selector form's receiver, so the
    // selector-only listing is the right scope for the table).
    {"Sum", 1, ""}, {"Min", 1, ""}, {"Max", 1, ""},
    {"Average", 1, ""}, {"Aggregate", 2, ""},
    // Ordering
    {"OrderBy", 1, ""}, {"OrderByDescending", 1, ""},
    {"ThenBy", 1, ""}, {"ThenByDescending", 1, ""},
    {"Reverse", 0, ""},
    // Slicing
    {"Take", 1, ""}, {"TakeWhile", 1, ""},
    {"Skip", 1, ""}, {"SkipWhile", 1, ""},
    // Combination
    {"Concat", 1, ""}, {"Zip", 2, ""},
    // Grouping
    {"GroupBy", 1, ""},
    // Materialization
    {"ToArray", 0, ""}, {"ToDictionary", 1, ""}, {"ToDictionary", 2, ""},

    // `extension IEnumerable<T> where T: hashable` block (stdlib §5 line 3585)
    {"Contains", 1, "where T: hashable"},
    {"Distinct", 0, "where T: hashable"},
    {"Union", 1, "where T: hashable"}, {"Intersect", 1, "where T: hashable"},
    {"Except", 1, "where T: hashable"},
    {"ToSet", 0, "where T: hashable"},

    // `extension IEnumerable<T> where T: IComparable<T>` block (stdlib §5.1)
    // The no-selector Min/Max overloads live here, not the main block. The
    // selector forms are in the main block (above).
    {"Min", 0, "where T: IComparable<T>"},
    {"Max", 0, "where T: IComparable<T>"},
    {"MinOrNull", 0, "where T: IComparable<T>"},
    {"MaxOrNull", 0, "where T: IComparable<T>"},

    // `extension IEnumerable<T> where T: INumber<T>` block (stdlib §5.1) —
    // the no-selector Sum() AND Average() live here. Average's no-selector
    // form is what `a.Average()` on `Array<int>` lowers to (the pre-#3726
    // table omitted it, so the arity gate falsely rejected `a.Average()`);
    // Sum() and the `where T: IComparable<T>` Min/Max above are the same
    // pattern — the no-selector form is a SEPARATE §5.1 row, not a duplicate
    // of the §5 selector row.
    {"Sum", 0, "where T: INumber<T>"},
    {"Average", 0, "where T: INumber<T>"},
};

// Look up the matching overload. With `arity < 0` the lookup returns the
// FIRST entry for the name (used to find its `where` clause when arity
// is not the question); otherwise the `(name, arity)` pair must match
// exactly. Returns nullptr if no overload matches.
inline const StdlibLinqOverload* stdlibLinqLookup(std::string_view name,
                                                  std::ptrdiff_t arity) {
    if (arity < 0) {
        for (const auto& e : kStdlibLinqOverloads)
            if (name == e.name) return &e;
        return nullptr;
    }
    for (const auto& e : kStdlibLinqOverloads)
        if (name == e.name && e.arity == arity) return &e;
    return nullptr;
}

inline bool stdlibLinqMemberMatches(std::string_view name,
                                    std::ptrdiff_t arity) {
    return stdlibLinqLookup(name, arity) != nullptr;
}

}  // namespace bezel::sema
