// RUN: %bezelc -c -o %t.o %s
//
// Helper artifact for self_import_dup_artifact_1730.bzl (issue #1730 repro
// shape 2: self-import checked against a SAME-NAMED dependency artifact).
// Declares `module Dup;` — the same name the sibling test's OWN file
// declares — so the sibling test can build this file into an artifact and
// pass it via `--import`, reproducing the exact issue #1730 shape
// (`module Dup; import Dup;` built against a `dup1.o` that also declares
// `module Dup;`). This file's own RUN line is an ordinary accept check (an
// unremarkable library, §2.4); its role as a dependency artifact is what the
// sibling test exercises.
module Dup;

public class Widget
  public method Ping(): int => 7;
end Widget;
