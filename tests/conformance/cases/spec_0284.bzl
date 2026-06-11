ClassDecl       ::= Visibility? Modifier* "class" Identifier GenericParams?
                    PrimaryCtorParams?
                    SerializedClassClause?
                    Inheritance?
                    GenericConstraints*
                    ClassBody
SerializedClassClause ::= "serialized" ( "tree" | "graph" | "cyclic" ) PolymorphicClause?
PolymorphicClause ::= "polymorphic" "(" PolyCase ("," PolyCase)* ")"
PolyCase ::= StringLiteral ":" TypeName
                              /* Optional polymorphic-dispatch registry on a
                                 base class that also carries a graph-shape
                                 assertion. Each PolyCase maps a wire
                                 discriminator string to a permitted concrete
                                 derived type. The set is closed and explicitly
                                 registered: there is no reflective discovery of
                                 subtypes (rationale §5.18, §5.19). The
                                 discriminator must be a non-empty literal and
                                 unique within the clause; each TypeName must
                                 name a type assignable to the declaring class
                                 and itself serialization-eligible under its own
                                 graph-shape clause. A base class with a
                                 PolymorphicClause may itself be concrete (it
                                 registers its own discriminator like any other
                                 case) or abstract (no self case).
                                 `polymorphic` is a contextual word here, not a
                                 reserved keyword. */
                              /* Class-level opt-in enabling serialization
                                 synthesis for the class (§9.8). Two assertions:
                                 `serialized tree` asserts the serialized graph
                                 is a tree (no two serialized edges alias the
                                 same object, no cycles); `serialized graph`
                                 asserts it is a directed acyclic graph (edges
                                 may alias a shared object, but there are no
                                 cycles); `serialized cyclic` asserts a general
                                 graph that may contain cycles. `tree` and
                                 `graph` are enforced dynamically: `tree` raises
                                 on any repeated object identity; `graph`
                                 preserves the first occurrence and binds later
                                 references to it, but raises on a cycle (a
                                 reference encountered while its own target is
                                 still being constructed). `cyclic` permits such
                                 a reference: it is deserialized via a two-phase
                                 construction (§9.8) and additionally requires
                                 every participating class's *construction* to
                                 be *self-contained* (§9.8 — the rule spans the
                                 constructor body, transitively chained
                                 `this(...)`/`base(...)` constructors, and
                                 field/property initializers), a
                                 statically-checked eligibility restriction; a
                                 class in a `cyclic` group whose construction is
                                 not self-contained is a compile error, not a
                                 runtime fault. `tree`, `graph`, and `cyclic`
                                 are contextual words here, not reserved
                                 keywords. Distinct from the member-level
                                 `SerializedClause` (§9.8 field-name
                                 customization) and from the deserializer's
                                 operation-scoped unknown-field policy (stdlib
                                 §2.15): graph shape is an intrinsic property of
                                 the type's serialization contract, so it is
                                 type-scoped, by the placement principle of
                                 rationale §5.19. */
                              /* The grammar admits modifier combinations the
                                 type system rejects: `static class C(args)`
                                 parses (Modifier* allows `static`,
                                 PrimaryCtorParams? is optional and present)
                                 but is rejected semantically — static classes
                                 cannot be instantiated and so cannot carry
                                 primary-ctor parameters (§11.7). */
ClassBody       ::= Member* "end" Identifier? ";"
Modifier        ::= "abstract" | "sealed" | "static"
                              /* Modifier* permits any combination; the
                                 semantically valid combinations are described
                                 in §11.7. `sealed abstract` is permitted
                                 (closed abstract class); other combinations
                                 with `static` are rejected. */
PrimaryCtorParams ::= "(" ParamList? ")"
Inheritance     ::= ":" InheritItem ("," InheritItem)*
InheritItem     ::= Type ("(" ArgList? ")")?
                              /* The grammar accepts the optional argument list
                                 on every InheritItem; semantically, the
                                 argument list is permitted only on the base
                                 class (the first item), not on interfaces. */

InterfaceDecl   ::= Visibility? "interface" Identifier GenericParams?
                    Inheritance?
                    GenericConstraints*
                    InterfaceMember* "end" Identifier? ";"

RecordDecl      ::= Visibility? "record" Identifier GenericParams?
                    PositionalFields?
                    Inheritance?
                    GenericConstraints*
                    ( ";" | RecordBody )
PositionalFields ::= "(" PositionalField ("," PositionalField)* ")"
PositionalField ::= Identifier ":" Type SerializedClause?
SerializedClause ::= "serialized" ( "skip" | SerializedName )
SerializedName   ::= StringLiteral? "optional"?   /* at least one of the two
                                                     must be present */
                              /* Compiler-recognized field-name customization
                                 for serialization synthesis (§9.8). NOT an
                                 attribute system: a closed vocabulary, no
                                 user extensibility, no runtime introspection.
                                 `serialized "name"` renames; `serialized
                                 optional` makes the field tolerant
                                 (omit-if-null on write, default-if-missing on
                                 read); `serialized "name" optional` combines
                                 the two; `serialized skip` excludes the field
                                 and is mutually exclusive with a name or
                                 `optional`. `skip` and `optional` are
                                 contextual words here, not reserved keywords;
                                 the `SerializedName` form requires at least
                                 one of the string or `optional` (a bare
                                 `serialized` is a syntax error). */
RecordBody      ::= Member* "end" Identifier? ";"

EnumDecl        ::= Visibility? "enum" Identifier EnumInheritance?
                    EnumConstants
                    EnumMember* "end" Identifier? ";"
EnumInheritance ::= ":" IntegerType ("," InheritItem)*
                  | ":" InheritItem ("," InheritItem)*
                              /* The first form is `enum E: int, IComparable<E>` —
                                 explicit underlying type followed by interfaces.
                                 The second form is `enum E: IComparable<E>` —
                                 default underlying type (int), interfaces only.
                                 The grammar disambiguates by whether the first
                                 token after `:` is an IntegerType keyword. */
EnumConstants   ::= EnumConstant ("," EnumConstant)* ";"
EnumConstant    ::= Identifier (":=" ConstExpr)? SerializedClause?
                              /* `serialized "name"` on an enum constant
                                 customizes that member's serialized name
                                 (§9.8). `serialized skip` and `serialized
                                 optional` are not admitted on enum constants
                                 — an enum value is a single member, not a
                                 field that can be omitted or defaulted; using
                                 them here is a compile error. */

ExtensionDecl   ::= Visibility? "extension" Type GenericConstraints*
                    ExtensionMember* "end" ";"

ExternBlock     ::= "extern" StringLiteral ("convention" StringLiteral)?
                    ExternDecl+ "end" ";"
ExternDecl      ::= Visibility? "method" Identifier "(" ParamList? ")"
                    (":" "owned"? Type)? ";"
                              /* `owned` on the return is permitted for
                                 declarations whose foreign function returns
                                 a class instance the Bezel caller takes
                                 ownership of; see §32.5 / §32.7. */
