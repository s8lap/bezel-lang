SourceFile      ::= ModuleDecl Import* TopDecl*
ModuleDecl      ::= "module" QualifiedName ";"
Import          ::= "import" QualifiedName ";"
QualifiedName   ::= Identifier ("." Identifier)*

TopDecl         ::= ClassDecl | InterfaceDecl | RecordDecl | EnumDecl
                  | ExtensionDecl | TypeAliasDecl | ExternBlock
TypeAliasDecl   ::= "type" Identifier GenericParams? "=" Type
                    GenericConstraints* ";"
