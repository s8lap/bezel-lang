// RUN: %bezelc -c -o %t.o %s
//
// Helper dependency artifact for xmod_static_interface_2294.bzl and
// xmod_static_interface_accept_2294.bzl (issue #2294 / spec §11.8, §11.2,
// §2.8.2). Declares `module SiLib2294;` exporting two interfaces with §11.8
// `static` members — one value-returning with a parameter, one unit-returning
// — so the sibling tests can exercise conformance across a real `--import`
// boundary. Its own RUN line is an unremarkable library accept (§2.4).
module SiLib2294;

public interface IPing
  static method Ping(n: int): int;
end IPing;

public interface IVoid
  static method Touch();
end IVoid;
