#!/usr/bin/env python3
"""check_nullable_string_producer_guard.py — ratchet: the §7.4 invariant that
lets codegen's string-producer recognizer ignore the receiver's DECLARED
nullability (#7139).

THE INVARIANT

  `Emitter::isPlusOneStringProducer` (src/codegen/LlvmBackend_Setup.cpp)
  classifies a §9.6 string-producing instance method's receiver as a +1
  PRODUCER without asking whether the receiver's DECLARED type is `string?`.
  That is sound only because sema REJECTS every member access on an
  un-narrowed nullable receiver (§7.4, v0.343): a `string?` receiver that
  reaches codegen has been narrowed non-null by §7.4, and §7.11 (v0.343)
  says a narrowed operand IS a non-nullable operand.

  Before #7139 the recognizer DID consult `tt->nullable`, so the two
  components disagreed about the receiver and every string-producing §9.6
  instance method leaked its producer's reference on a narrowed `string?`
  receiver (3320 B in 80 allocations over a 40-iteration loop).

  The hazard this ratchet guards is the OTHER direction: a future sema
  change that admits an un-narrowed `string?` receiver would make the
  codegen arm classify a BORROW as a +1 producer, i.e. an OVER-RELEASE,
  with no diagnostic and no test failure.

THE CHECKS

  T1  codegen: the producer recognizer's receiver type test does NOT consult
      nullability, and there is exactly ONE such test in the file. A second
      one is a new recognition site and needs a human look.
  T2  codegen: the `?.` METHOD spelling is a DIFFERENT AST node and must not
      enter the recognizer as a string producer. The recognizer's `?.` call
      arm admits a fixed set of primitive receivers and EXCLUDES `string` on
      purpose ("Bool (sentinel), String (identity) and Object (receiver-
      shape dependent) stay excluded, as for the `.` form") — a `?.` string
      method's receiver is genuinely nullable, so admitting it would
      over-release the borrow.
  T3  codegen: the reason travels with the code — the recognizer's body
      cites §7.4 and §7.11.
  T4  sema: every nullable-receiver rejection in src/sema names §7.4, and
      the NUMBER of those rejections equals the banked baseline. A removal
      is the hazard; an addition is a strengthening; both are worth a human
      look, so the ratchet fires on either and is re-banked deliberately.
  T5  test: the #7139 regression file must exist and must still assert the
      compile-time rejection, so the sema side's protection is PINNED.

BUILD-FREE SOURCE ratchet: it reads sources and documents as text and
compiles nothing.

Exit codes:
  0  the invariant holds
  1  it does not
  2  the checker could not parse its inputs (FAIL CLOSED — never a green)

Usage: scripts/check_nullable_string_producer_guard.py [--strict] [--self-test]
       [--update]
"""

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CODEGEN = os.path.join(REPO, "src", "codegen", "LlvmBackend_Setup.cpp")
SEMA_DIR = os.path.join(REPO, "src", "sema")
TEST_FILE = os.path.join(
    REPO, "tests", "codegen",
    "narrowed_nullable_string_receiver_producer_leak_7139_test.cpp")

# The banked number of nullable-receiver rejection sites in src/sema.
# Banked on the tree at 0af776433 (R35). Re-bank with --update ONLY after
# reading every site that changed.
BASELINE_REJECTION_SITES = 7

# The source spells the citation two ways: `§7.4` and `§""7.4` (the C++
# string literal is split so the section sign needs no encoding).
CITE_74 = re.compile(r'§(?:"")?7\.4')
CITE_711 = re.compile(r'§(?:"")?7\.11')

# The producer recognizer's definition.
SIG_RE = r"bool\s+Emitter::isPlusOneStringProducer\s*\("

# The receiver type test: `if (tt && tt->kind == ...::Primitive && <cond>) {`.
# `cond` is captured so the checker can ask whether it consults nullability.
TYPE_TEST_RE = re.compile(
    r"if\s*\(\s*tt\s*&&\s*tt->kind\s*==\s*sema::TypeRef::Kind::Primitive\s*&&"
    r"(?P<cond>[^;{}]*?)\)\s*\{")

# The `?.` spelling's AST node, on a CALL's callee. The recognizer's `?.`
# CALL arm is the one whose cast argument is the callee.
NULLCOND_CALLEE_RE = re.compile(
    r"dynamic_cast\s*<\s*const\s+ast::NullCondMemberExpr\s*\*>\s*"
    r"\(\s*call->callee", re.S)

# The primitive switch inside that `?.` call arm: the set of receiver
# primitives for which the `?.` call is a producer.
PRIM_SWITCH_RE = re.compile(r"switch\s*\(\s*tt->primitive\s*\)\s*\{")

# The diagnostic phrase of a nullable-receiver rejection.
REJECT_PHRASE = "requires narrowing"


def die(msg, code=2):
    sys.stderr.write("check_nullable_string_producer_guard: %s\n" % msg)
    sys.exit(code)


def read(path):
    try:
        with open(path, encoding="utf-8") as fh:
            return fh.read()
    except OSError as exc:
        die("cannot read %s: %s" % (path, exc))


def function_span(text):
    """(start, end, body) of `isPlusOneStringProducer`, or None.

    The body ends at the first line that is exactly `}` at column 0, which
    is how a member function definition ends in this file. Brace counting
    is NOT used over the whole function: string literals in this file
    contain braces.
    """
    m = re.search(SIG_RE, text)
    if not m:
        return None
    lines = text[m.start():].split("\n")
    for idx, line in enumerate(lines[1:], start=1):
        if line.rstrip() == "}":
            body = "\n".join(lines[:idx + 1])
            return (m.start(), m.start() + len(body), body)
    return None


def brace_block(text, open_index):
    """The text between the braces at `open_index` and its match."""
    depth = 0
    for j in range(open_index, len(text)):
        if text[j] == "{":
            depth += 1
        elif text[j] == "}":
            depth -= 1
            if depth == 0:
                return text[open_index + 1:j]
    return None


def nullcond_call_switch(body):
    """The primitive switch of the recognizer's `?.` CALL arm, or None.

    The `?.` property arm (a `q?.W` read) is a different thing and is
    deliberately classified on its own; only the CALL arm is in scope
    here, and it is identified by the cast's argument being the callee.
    """
    m = NULLCOND_CALLEE_RE.search(body)
    if not m:
        return None
    sw = PRIM_SWITCH_RE.search(body, m.end())
    if not sw:
        return None
    open_index = body.index("{", sw.start())
    return brace_block(body, open_index)


def check_codegen(text, problems):
    """T1, T2, T3."""
    span = function_span(text)
    if span is None:
        die("no `Emitter::isPlusOneStringProducer` definition found in %s"
            % CODEGEN)
    start, end, body = span

    # T2 — the `?.` CALL arm must EXCLUDE the string primitive.
    switch = nullcond_call_switch(body)
    if switch is None:
        die("the `?.` call arm's primitive switch was not found in "
            "`isPlusOneStringProducer` — the checker would compare nothing")
    if re.search(r"::String\b", switch):
        problems.append(
            "codegen: the recognizer's `?.` CALL arm admits the STRING "
            "primitive. A `?.` method call's receiver is GENUINELY nullable, "
            "so classifying it as a +1 producer over-releases the borrow. The "
            "arm excludes String on purpose.")

    # T3 — the reason travels with the code.
    for label, cite in (("§7.4", CITE_74), ("§7.11", CITE_711)):
        if not cite.search(body):
            problems.append(
                "codegen: `isPlusOneStringProducer`'s body does not cite %s. "
                "The reason the receiver's DECLARED nullability is ignored "
                "must travel with the code, or the next reader will treat the "
                "absence of a nullability test as an oversight." % label)

    # T1 — exactly one receiver type test, and it must not consult
    # nullability.
    tests = list(TYPE_TEST_RE.finditer(text))
    if not tests:
        die("no receiver type test (`tt && tt->kind == ...::Primitive`) found "
            "in %s — the checker would compare nothing" % CODEGEN)
    if len(tests) > 1:
        problems.append(
            "codegen: %d receiver type tests (`tt && tt->kind == "
            "...::Primitive`) found; the banked count is 1. A second "
            "producer-recognition site needs a human look: it may consult "
            "nullability, and the recognizer and the lowering must agree "
            "about every one of them." % len(tests))
    for m in tests:
        if not (start <= m.start() < end):
            problems.append(
                "codegen: a receiver type test at line %d is OUTSIDE "
                "`isPlusOneStringProducer`; the banked count is one, inside "
                "it." % (text[:m.start()].count("\n") + 1))
            continue
        if re.search(r"nullab", m.group("cond"), re.I):
            problems.append(
                "codegen: the producer recognizer's receiver type test "
                "CONSULTS the receiver's nullability (`%s`). That is the "
                "pre-#7139 shape: the recognizer and the lowering then "
                "disagree about the receiver, and every string-producing "
                "§9.6 instance method leaks its producer's reference on a "
                "§7.4-narrowed `string?` receiver."
                % " ".join(m.group("cond").split()))
    return problems


def check_sema(texts, problems):
    """T4 — every nullable-receiver rejection names §7.4, and the count of
    them equals the banked baseline."""
    sites = 0
    for path, text in texts:
        lines = text.split("\n")
        for i, line in enumerate(lines):
            pos = line.find(REJECT_PHRASE)
            if pos < 0:
                continue
            # A mention inside a comment is not a rejection site.
            if "//" in line[:pos]:
                continue
            sites += 1
            window = "\n".join(lines[max(0, i - 8):i + 9])
            if not CITE_74.search(window):
                problems.append(
                    "sema: the nullable-receiver rejection at %s:%d does not "
                    "name §7.4. Every rejection of a member access on an "
                    "un-narrowed nullable receiver must name the rule it "
                    "enforces, or a reader cannot tell it from a rejection "
                    "of some other kind."
                    % (os.path.relpath(path, REPO), i + 1))
    if sites != BASELINE_REJECTION_SITES:
        problems.append(
            "sema: %d nullable-receiver rejection sites in src/sema; the "
            "banked baseline is %d. A REMOVAL is the hazard #7139's invariant "
            "depends on (a member access on an un-narrowed `string?` receiver "
            "would reach codegen, which would over-release the borrow); an "
            "ADDITION is a strengthening. Read every changed site, then "
            "re-bank deliberately."
            % (sites, BASELINE_REJECTION_SITES))
    return problems


def check_test(text, problems):
    """T5 — the §7.4 rejection is pinned by a test."""
    if "requires narrowing it to non-null" not in text:
        problems.append(
            "test: %s does not assert the compile-time rejection of a member "
            "access on an un-narrowed nullable receiver. The sema side of "
            "the #7139 invariant must stay pinned, or removing the §7.4 "
            "gate would produce an over-release with no test failure."
            % os.path.relpath(TEST_FILE, REPO))
    return problems


def check_all(code_text, sema_texts, test_text):
    problems = []
    check_codegen(code_text, problems)
    check_sema(sema_texts, problems)
    check_test(test_text, problems)
    return problems


def real_inputs():
    code_text = read(CODEGEN)
    sema_texts = [(os.path.join(SEMA_DIR, f),
                   read(os.path.join(SEMA_DIR, f)))
                  for f in sorted(os.listdir(SEMA_DIR)) if f.endswith(".cpp")]
    test_text = read(TEST_FILE)
    return code_text, sema_texts, test_text


def self_test():
    """Prove the checker FIRES on synthetic mutations.

    A checker that cannot detect a mutation proves nothing when it reports
    a match, so the gate runs this first.
    """
    code_text, sema_texts, test_text = real_inputs()

    if check_all(code_text, sema_texts, test_text):
        die("SELF-TEST FAILED: the real tree is not clean", code=2)

    # M1 — the pre-#7139 shape: the type test consults nullability.
    m = TYPE_TEST_RE.search(code_text)
    if not m:
        die("SELF-TEST FAILED: no receiver type test to mutate", code=2)
    mutated = (code_text[:m.start("cond")] + m.group("cond") +
               " && !tt->nullable" + code_text[m.end("cond"):])
    if not check_codegen(mutated, []):
        die("SELF-TEST FAILED: a type test that consults nullability was NOT "
            "detected")
    if check_codegen(code_text, []):
        die("SELF-TEST FAILED: the real codegen text was reported as a "
            "mismatch")

    # M2 — the `?.` call arm admits the string primitive.
    span = function_span(code_text)
    if span is None:
        die("SELF-TEST FAILED: no recognizer to mutate", code=2)
    body = span[2]
    sw = PRIM_SWITCH_RE.search(body)
    if not sw:
        die("SELF-TEST FAILED: no primitive switch to mutate", code=2)
    mutated_body = (body[:sw.end()] + " case P::String:" + body[sw.end():])
    mutated = (code_text[:span[0]] + mutated_body + code_text[span[1]:])
    if not check_codegen(mutated, []):
        die("SELF-TEST FAILED: the `?.` call arm admitting String was NOT "
            "detected")

    # M3 — the reason stops travelling with the code.
    mutated = code_text.replace("§7.11", "§7.99")
    if not check_codegen(mutated, []):
        die("SELF-TEST FAILED: a dropped §7.11 citation was NOT detected")

    # M4 — a sema rejection site is removed.
    victim = None
    for path, text in sema_texts:
        lines = text.split("\n")
        for i, line in enumerate(lines):
            pos = line.find(REJECT_PHRASE)
            if pos >= 0 and "//" not in line[:pos]:
                victim = (path, text, i)
                break
        if victim:
            break
    if victim is None:
        die("SELF-TEST FAILED: no sema rejection site to mutate", code=2)
    vpath, vtext, vline = victim
    vlines = vtext.split("\n")
    vlines[vline] = vlines[vline].replace(REJECT_PHRASE, "requires narrowinG")
    mutated_sema = [(p, "\n".join(vlines) if p == vpath else t)
                    for (p, t) in sema_texts]
    if not check_sema(mutated_sema, []):
        die("SELF-TEST FAILED: a removed sema rejection site was NOT "
            "detected")
    if check_sema(sema_texts, []):
        die("SELF-TEST FAILED: the real sema text was reported as a "
            "mismatch")

    # M5 — the test stops pinning the rejection.
    if not check_test(test_text.replace("requires narrowing it to non-null",
                                        "requires narrowing it to non-NULL"),
                      []):
        die("SELF-TEST FAILED: an unpinned §7.4 rejection was NOT detected")

    print("check_nullable_string_producer_guard --self-test: OK (a "
          "nullability consultation, a `?.` call arm admitting String, a "
          "dropped §7.11 citation, a removed sema rejection and an unpinned "
          "test are all detected; the real tree is in agreement)")
    return 0


def update_baseline():
    """Re-bank the rejection-site count. Deliberate: the caller must have
    read every changed site."""
    sema_texts = []
    for f in sorted(os.listdir(SEMA_DIR)):
        if f.endswith(".cpp"):
            sema_texts.append((os.path.join(SEMA_DIR, f),
                               read(os.path.join(SEMA_DIR, f))))
    sites = 0
    for path, text in sema_texts:
        for line in text.split("\n"):
            pos = line.find(REJECT_PHRASE)
            if pos >= 0 and "//" not in line[:pos]:
                sites += 1
    path = os.path.abspath(__file__)
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    new, n = re.subn(r"BASELINE_REJECTION_SITES\s*=\s*\d+",
                     "BASELINE_REJECTION_SITES = %d" % sites, src)
    if n != 1:
        die("cannot find BASELINE_REJECTION_SITES in %s" % path)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(new)
    print("check_nullable_string_producer_guard: re-banked "
          "BASELINE_REJECTION_SITES = %d" % sites)
    return 0


def main():
    if "--self-test" in sys.argv:
        return self_test()
    if "--update" in sys.argv:
        return update_baseline()

    code_text, sema_texts, test_text = real_inputs()
    problems = check_all(code_text, sema_texts, test_text)
    if problems:
        out = ["check_nullable_string_producer_guard: the §7.4 / #7139 "
               "invariant does not hold"] + ["  " + p for p in problems]
        sys.stderr.write("\n".join(out) + "\n")
        return 1

    print("check_nullable_string_producer_guard: clean (the producer "
          "recognizer ignores the receiver's declared nullability; %d §7.4 "
          "nullable-receiver rejections in src/sema; the `?.` call arm "
          "excludes String; the rejection is pinned by a test)"
          % BASELINE_REJECTION_SITES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
