//===--- Stmt.h - Classes for representing statements -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines the Stmt interface and subclasses.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_STMT_H
#define LLVM_CLANG_AST_STMT_H

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/AST/StmtIterator.h"
#include "clang/AST/DeclGroup.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/iterator.h"
#include "llvm/Bitcode/SerializationFwd.h"
#include "clang/AST/ASTContext.h"
#include <string>
using llvm::dyn_cast_or_null;

namespace clang {
  class ASTContext;
  class Expr;
  class Decl;
  class ParmVarDecl;
  class QualType;
  class IdentifierInfo;
  class SourceManager;
  class StringLiteral;
  class SwitchStmt;
  class PrinterHelper;
  
  //===----------------------------------------------------------------------===//
  // ExprIterator - Iterators for iterating over Stmt* arrays that contain
  //  only Expr*.  This is needed because AST nodes use Stmt* arrays to store
  //  references to children (to be compatible with StmtIterator).
  //===----------------------------------------------------------------------===//
  
  class Stmt;
  class Expr;
  
  class ExprIterator {
    Stmt** I;
  public:
    ExprIterator(Stmt** i) : I(i) {}
    ExprIterator() : I(0) {}    
    ExprIterator& operator++() { ++I; return *this; }
    ExprIterator operator-(size_t i) { return I-i; }
    ExprIterator operator+(size_t i) { return I+i; }
    Expr* operator[](size_t idx);
    // FIXME: Verify that this will correctly return a signed distance.
    signed operator-(const ExprIterator& R) const { return I - R.I; }
    Expr* operator*() const;
    Expr* operator->() const;
    bool operator==(const ExprIterator& R) const { return I == R.I; }
    bool operator!=(const ExprIterator& R) const { return I != R.I; }
    bool operator>(const ExprIterator& R) const { return I > R.I; }
    bool operator>=(const ExprIterator& R) const { return I >= R.I; }
  };
  
  class ConstExprIterator {
    Stmt* const * I;
  public:
    ConstExprIterator(Stmt* const* i) : I(i) {}
    ConstExprIterator() : I(0) {}    
    ConstExprIterator& operator++() { ++I; return *this; }
    ConstExprIterator operator+(size_t i) { return I+i; }
    ConstExprIterator operator-(size_t i) { return I-i; }
    const Expr * operator[](size_t idx) const;
    signed operator-(const ConstExprIterator& R) const { return I - R.I; }
    const Expr * operator*() const;
    const Expr * operator->() const;
    bool operator==(const ConstExprIterator& R) const { return I == R.I; }
    bool operator!=(const ConstExprIterator& R) const { return I != R.I; }
    bool operator>(const ConstExprIterator& R) const { return I > R.I; }
    bool operator>=(const ConstExprIterator& R) const { return I >= R.I; }
  }; 
  
//===----------------------------------------------------------------------===//
// AST classes for statements.
//===----------------------------------------------------------------------===//
  
/// Stmt - This represents one statement.
///
class Stmt {
public:
  enum StmtClass {
    NoStmtClass = 0,
#define STMT(CLASS, PARENT) CLASS##Class,
#define FIRST_STMT(CLASS) firstStmtConstant = CLASS##Class,
#define LAST_STMT(CLASS) lastStmtConstant = CLASS##Class,
#define FIRST_EXPR(CLASS) firstExprConstant = CLASS##Class,
#define LAST_EXPR(CLASS) lastExprConstant = CLASS##Class
#include "clang/AST/StmtNodes.def"
};
private:
  const StmtClass sClass;

  // Make vanilla 'new' and 'delete' illegal for Stmts.
protected:
  void* operator new(size_t bytes) throw() {
    assert(0 && "Stmts cannot be allocated with regular 'new'.");
    return 0;
  }
  void operator delete(void* data) throw() {
    assert(0 && "Stmts cannot be released with regular 'delete'.");
  }
  
public:
  // Only allow allocation of Stmts using the allocator in ASTContext
  // or by doing a placement new.  
  void* operator new(size_t bytes, ASTContext& C,
                     unsigned alignment = 16) throw() {
    return ::operator new(bytes, C, alignment);
  }
  
  void* operator new(size_t bytes, ASTContext* C,
                     unsigned alignment = 16) throw() {
    return ::operator new(bytes, *C, alignment);
  }
  
  void* operator new(size_t bytes, void* mem) throw() {
    return mem;
  }

  void operator delete(void*, ASTContext&, unsigned) throw() { }
  void operator delete(void*, ASTContext*, unsigned) throw() { }
  void operator delete(void*, std::size_t) throw() { }
  void operator delete(void*, void*) throw() { }

protected:
  /// DestroyChildren - Invoked by destructors of subclasses of Stmt to
  ///  recursively release child AST nodes.
  void DestroyChildren(ASTContext& Ctx);
  
public:
  Stmt(StmtClass SC) : sClass(SC) { 
    if (Stmt::CollectingStats()) Stmt::addStmtClass(SC);
  }
  virtual ~Stmt() {}
  
  virtual void Destroy(ASTContext &Ctx);

  StmtClass getStmtClass() const { return sClass; }
  const char *getStmtClassName() const;
  
  /// SourceLocation tokens are not useful in isolation - they are low level
  /// value objects created/interpreted by SourceManager. We assume AST
  /// clients will have a pointer to the respective SourceManager.
  virtual SourceRange getSourceRange() const = 0;
  SourceLocation getLocStart() const { return getSourceRange().getBegin(); }
  SourceLocation getLocEnd() const { return getSourceRange().getEnd(); }

  // global temp stats (until we have a per-module visitor)
  static void addStmtClass(const StmtClass s);
  static bool CollectingStats(bool enable=false);
  static void PrintStats();

  /// dump - This does a local dump of the specified AST fragment.  It dumps the
  /// specified node and a few nodes underneath it, but not the whole subtree.
  /// This is useful in a debugger.
  void dump() const;
  void dump(SourceManager &SM) const;

  /// dumpAll - This does a dump of the specified AST fragment and all subtrees.
  void dumpAll() const;
  void dumpAll(SourceManager &SM) const;

  /// dumpPretty/printPretty - These two methods do a "pretty print" of the AST
  /// back to its original source language syntax.
  void dumpPretty() const;
  void printPretty(llvm::raw_ostream &OS, PrinterHelper* = NULL, unsigned = 0,
                   bool NoIndent=false) const;
  
  /// viewAST - Visualize an AST rooted at this Stmt* using GraphViz.  Only
  ///   works on systems with GraphViz (Mac OS X) or dot+gv installed.
  void viewAST() const;
  
  // Implement isa<T> support.
  static bool classof(const Stmt *) { return true; }  
  
  /// hasImplicitControlFlow - Some statements (e.g. short circuited operations)
  ///  contain implicit control-flow in the order their subexpressions
  ///  are evaluated.  This predicate returns true if this statement has
  ///  such implicit control-flow.  Such statements are also specially handled
  ///  within CFGs.
  bool hasImplicitControlFlow() const;

  /// Child Iterators: All subclasses must implement child_begin and child_end
  ///  to permit easy iteration over the substatements/subexpessions of an
  ///  AST node.  This permits easy iteration over all nodes in the AST.
  typedef StmtIterator       child_iterator;
  typedef ConstStmtIterator  const_child_iterator;
  
  virtual child_iterator child_begin() = 0;
  virtual child_iterator child_end()   = 0;
  
  const_child_iterator child_begin() const {
    return const_child_iterator(const_cast<Stmt*>(this)->child_begin());
  }
  
  const_child_iterator child_end() const {
    return const_child_iterator(const_cast<Stmt*>(this)->child_end());
  }

  /// \brief A placeholder type used to construct an empty shell of a
  /// type, that will be filled in later (e.g., by some
  /// de-serialization).
  struct EmptyShell { };

  void Emit(llvm::Serializer& S) const;
  static Stmt* Create(llvm::Deserializer& D, ASTContext& C);

  virtual void EmitImpl(llvm::Serializer& S) const {
    // This method will eventually be a pure-virtual function.
    assert (false && "Not implemented.");
  }
};

/// DeclStmt - Adaptor class for mixing declarations with statements and
/// expressions. For example, CompoundStmt mixes statements, expressions
/// and declarations (variables, types). Another example is ForStmt, where 
/// the first statement can be an expression or a declaration.
///
class DeclStmt : public Stmt {
  DeclGroupRef DG;
  SourceLocation StartLoc, EndLoc;
public:
  DeclStmt(DeclGroupRef dg, SourceLocation startLoc, 
           SourceLocation endLoc) : Stmt(DeclStmtClass), DG(dg),
                                    StartLoc(startLoc), EndLoc(endLoc) {}
  
  virtual void Destroy(ASTContext& Ctx);

  /// isSingleDecl - This method returns true if this DeclStmt refers
  /// to a single Decl.
  bool isSingleDecl() const {
    return DG.isSingleDecl();
  }
 
  const Decl *getSingleDecl() const { return DG.getSingleDecl(); }
  Decl *getSingleDecl() { return DG.getSingleDecl(); }  
  
  const DeclGroupRef getDeclGroup() const { return DG; }
  DeclGroupRef getDeclGroup() { return DG; }

  SourceLocation getStartLoc() const { return StartLoc; }
  SourceLocation getEndLoc() const { return EndLoc; }
  
  SourceRange getSourceRange() const {
    return SourceRange(StartLoc, EndLoc);
  }
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == DeclStmtClass; 
  }
  static bool classof(const DeclStmt *) { return true; }
  
  // Iterators over subexpressions.
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  typedef DeclGroupRef::iterator decl_iterator;
  typedef DeclGroupRef::const_iterator const_decl_iterator;
  
  decl_iterator decl_begin() { return DG.begin(); }
  decl_iterator decl_end() { return DG.end(); }
  const_decl_iterator decl_begin() const { return DG.begin(); }
  const_decl_iterator decl_end() const { return DG.end(); }
  
  // Serialization.  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static DeclStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// NullStmt - This is the null statement ";": C99 6.8.3p3.
///
class NullStmt : public Stmt {
  SourceLocation SemiLoc;
public:
  NullStmt(SourceLocation L) : Stmt(NullStmtClass), SemiLoc(L) {}

  SourceLocation getSemiLoc() const { return SemiLoc; }

  virtual SourceRange getSourceRange() const { return SourceRange(SemiLoc); }
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == NullStmtClass; 
  }
  static bool classof(const NullStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static NullStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// CompoundStmt - This represents a group of statements like { stmt stmt }.
///
class CompoundStmt : public Stmt {
  Stmt** Body;
  unsigned NumStmts;
  SourceLocation LBracLoc, RBracLoc;
public:
  CompoundStmt(ASTContext& C, Stmt **StmtStart, unsigned numStmts, 
                             SourceLocation LB, SourceLocation RB)
  : Stmt(CompoundStmtClass), NumStmts(numStmts), LBracLoc(LB), RBracLoc(RB) {
    if (NumStmts == 0) {
      Body = 0;
      return;
    }
  
    Body = new (C) Stmt*[NumStmts];
    memcpy(Body, StmtStart, numStmts * sizeof(*Body));
  }           
  
  bool body_empty() const { return NumStmts == 0; }
  
  typedef Stmt** body_iterator;
  body_iterator body_begin() { return Body; }
  body_iterator body_end() { return Body + NumStmts; }
  Stmt *body_back() { return NumStmts ? Body[NumStmts-1] : 0; }

  typedef Stmt* const * const_body_iterator;
  const_body_iterator body_begin() const { return Body; }
  const_body_iterator body_end() const { return Body + NumStmts; }
  const Stmt *body_back() const { return NumStmts ? Body[NumStmts-1] : 0; }

  typedef std::reverse_iterator<body_iterator> reverse_body_iterator;
  reverse_body_iterator body_rbegin() {
    return reverse_body_iterator(body_end());
  }
  reverse_body_iterator body_rend() {
    return reverse_body_iterator(body_begin());
  }

  typedef std::reverse_iterator<const_body_iterator>
          const_reverse_body_iterator;

  const_reverse_body_iterator body_rbegin() const {
    return const_reverse_body_iterator(body_end());
  }
  
  const_reverse_body_iterator body_rend() const {
    return const_reverse_body_iterator(body_begin());
  }
    
  virtual SourceRange getSourceRange() const { 
    return SourceRange(LBracLoc, RBracLoc); 
  }
  
  SourceLocation getLBracLoc() const { return LBracLoc; }
  SourceLocation getRBracLoc() const { return RBracLoc; }
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == CompoundStmtClass; 
  }
  static bool classof(const CompoundStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static CompoundStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

// SwitchCase is the base class for CaseStmt and DefaultStmt,
class SwitchCase : public Stmt {
protected:
  // A pointer to the following CaseStmt or DefaultStmt class,
  // used by SwitchStmt.
  SwitchCase *NextSwitchCase;

  SwitchCase(StmtClass SC) : Stmt(SC), NextSwitchCase(0) {}
  
public:
  const SwitchCase *getNextSwitchCase() const { return NextSwitchCase; }

  SwitchCase *getNextSwitchCase() { return NextSwitchCase; }

  void setNextSwitchCase(SwitchCase *SC) { NextSwitchCase = SC; }

  Stmt *getSubStmt() { return v_getSubStmt(); }

  virtual SourceRange getSourceRange() const { return SourceRange(); }
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == CaseStmtClass || 
    T->getStmtClass() == DefaultStmtClass;
  }
  static bool classof(const SwitchCase *) { return true; }
protected:
  virtual Stmt* v_getSubStmt() = 0;  
};

class CaseStmt : public SwitchCase {
  enum { SUBSTMT, LHS, RHS, END_EXPR };
  Stmt* SubExprs[END_EXPR];  // The expression for the RHS is Non-null for 
                             // GNU "case 1 ... 4" extension
  SourceLocation CaseLoc;
  virtual Stmt* v_getSubStmt() { return getSubStmt(); }
public:
  CaseStmt(Expr *lhs, Expr *rhs, SourceLocation caseLoc) 
    : SwitchCase(CaseStmtClass) {
    SubExprs[SUBSTMT] = 0;
    SubExprs[LHS] = reinterpret_cast<Stmt*>(lhs);
    SubExprs[RHS] = reinterpret_cast<Stmt*>(rhs);
    CaseLoc = caseLoc;
  }
  
  SourceLocation getCaseLoc() const { return CaseLoc; }
  
  Expr *getLHS() { return reinterpret_cast<Expr*>(SubExprs[LHS]); }
  Expr *getRHS() { return reinterpret_cast<Expr*>(SubExprs[RHS]); }
  Stmt *getSubStmt() { return SubExprs[SUBSTMT]; }
  const Expr *getLHS() const { 
    return reinterpret_cast<const Expr*>(SubExprs[LHS]); 
  }
  const Expr *getRHS() const { 
    return reinterpret_cast<const Expr*>(SubExprs[RHS]); 
  }
  const Stmt *getSubStmt() const { return SubExprs[SUBSTMT]; }

  void setSubStmt(Stmt *S) { SubExprs[SUBSTMT] = S; }
  void setLHS(Expr *Val) { SubExprs[LHS] = reinterpret_cast<Stmt*>(Val); }
  void setRHS(Expr *Val) { SubExprs[RHS] = reinterpret_cast<Stmt*>(Val); }
  
  
  virtual SourceRange getSourceRange() const {
    // Handle deeply nested case statements with iteration instead of recursion.
    const CaseStmt *CS = this;
    while (const CaseStmt *CS2 = dyn_cast<CaseStmt>(CS->getSubStmt()))
      CS = CS2;
    
    return SourceRange(CaseLoc, CS->getSubStmt()->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == CaseStmtClass; 
  }
  static bool classof(const CaseStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static CaseStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

class DefaultStmt : public SwitchCase {
  Stmt* SubStmt;
  SourceLocation DefaultLoc;
  virtual Stmt* v_getSubStmt() { return getSubStmt(); }
public:
  DefaultStmt(SourceLocation DL, Stmt *substmt) : 
    SwitchCase(DefaultStmtClass), SubStmt(substmt), DefaultLoc(DL) {}
    
  Stmt *getSubStmt() { return SubStmt; }
  const Stmt *getSubStmt() const { return SubStmt; }
    
  SourceLocation getDefaultLoc() const { return DefaultLoc; }

  virtual SourceRange getSourceRange() const { 
    return SourceRange(DefaultLoc, SubStmt->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == DefaultStmtClass; 
  }
  static bool classof(const DefaultStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static DefaultStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

class LabelStmt : public Stmt {
  IdentifierInfo *Label;
  Stmt *SubStmt;
  SourceLocation IdentLoc;
public:
  LabelStmt(SourceLocation IL, IdentifierInfo *label, Stmt *substmt) 
    : Stmt(LabelStmtClass), Label(label), 
      SubStmt(substmt), IdentLoc(IL) {}
  
  SourceLocation getIdentLoc() const { return IdentLoc; }
  IdentifierInfo *getID() const { return Label; }
  const char *getName() const;
  Stmt *getSubStmt() { return SubStmt; }
  const Stmt *getSubStmt() const { return SubStmt; }

  void setIdentLoc(SourceLocation L) { IdentLoc = L; }
  void setSubStmt(Stmt *SS) { SubStmt = SS; }

  virtual SourceRange getSourceRange() const { 
    return SourceRange(IdentLoc, SubStmt->getLocEnd()); 
  }  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == LabelStmtClass; 
  }
  static bool classof(const LabelStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static LabelStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};


/// IfStmt - This represents an if/then/else.
///
class IfStmt : public Stmt {
  enum { COND, THEN, ELSE, END_EXPR };
  Stmt* SubExprs[END_EXPR];
  SourceLocation IfLoc;
public:
  IfStmt(SourceLocation IL, Expr *cond, Stmt *then, Stmt *elsev = 0) 
    : Stmt(IfStmtClass)  {
    SubExprs[COND] = reinterpret_cast<Stmt*>(cond);
    SubExprs[THEN] = then;
    SubExprs[ELSE] = elsev;
    IfLoc = IL;
  }
  
  const Expr *getCond() const { return reinterpret_cast<Expr*>(SubExprs[COND]);}
  const Stmt *getThen() const { return SubExprs[THEN]; }
  const Stmt *getElse() const { return SubExprs[ELSE]; }

  Expr *getCond() { return reinterpret_cast<Expr*>(SubExprs[COND]); }
  Stmt *getThen() { return SubExprs[THEN]; }
  Stmt *getElse() { return SubExprs[ELSE]; }

  virtual SourceRange getSourceRange() const { 
    if (SubExprs[ELSE])
      return SourceRange(IfLoc, SubExprs[ELSE]->getLocEnd());
    else
      return SourceRange(IfLoc, SubExprs[THEN]->getLocEnd());
  }
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == IfStmtClass; 
  }
  static bool classof(const IfStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static IfStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// SwitchStmt - This represents a 'switch' stmt.
///
class SwitchStmt : public Stmt {
  enum { COND, BODY, END_EXPR };
  Stmt* SubExprs[END_EXPR];  
  // This points to a linked list of case and default statements.
  SwitchCase *FirstCase;
  SourceLocation SwitchLoc;
public:
  SwitchStmt(Expr *cond) : Stmt(SwitchStmtClass), FirstCase(0) {
      SubExprs[COND] = reinterpret_cast<Stmt*>(cond);
      SubExprs[BODY] = NULL;
    }
  
  const Expr *getCond() const { return reinterpret_cast<Expr*>(SubExprs[COND]);}
  const Stmt *getBody() const { return SubExprs[BODY]; }
  const SwitchCase *getSwitchCaseList() const { return FirstCase; }

  Expr *getCond() { return reinterpret_cast<Expr*>(SubExprs[COND]);}
  Stmt *getBody() { return SubExprs[BODY]; }
  SwitchCase *getSwitchCaseList() { return FirstCase; }

  void setBody(Stmt *S, SourceLocation SL) { 
    SubExprs[BODY] = S; 
    SwitchLoc = SL;
  }  
  void addSwitchCase(SwitchCase *SC) {
    assert(!SC->getNextSwitchCase() && "case/default already added to a switch");
    SC->setNextSwitchCase(FirstCase);
    FirstCase = SC;
  }
  virtual SourceRange getSourceRange() const { 
    return SourceRange(SwitchLoc, SubExprs[BODY]->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == SwitchStmtClass; 
  }
  static bool classof(const SwitchStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static SwitchStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};


/// WhileStmt - This represents a 'while' stmt.
///
class WhileStmt : public Stmt {
  enum { COND, BODY, END_EXPR };
  Stmt* SubExprs[END_EXPR];
  SourceLocation WhileLoc;
public:
  WhileStmt(Expr *cond, Stmt *body, SourceLocation WL) : Stmt(WhileStmtClass) {
    SubExprs[COND] = reinterpret_cast<Stmt*>(cond);
    SubExprs[BODY] = body;
    WhileLoc = WL;
  }
  
  Expr *getCond() { return reinterpret_cast<Expr*>(SubExprs[COND]); }
  const Expr *getCond() const { return reinterpret_cast<Expr*>(SubExprs[COND]);}
  Stmt *getBody() { return SubExprs[BODY]; }
  const Stmt *getBody() const { return SubExprs[BODY]; }

  virtual SourceRange getSourceRange() const { 
    return SourceRange(WhileLoc, SubExprs[BODY]->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == WhileStmtClass; 
  }
  static bool classof(const WhileStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static WhileStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// DoStmt - This represents a 'do/while' stmt.
///
class DoStmt : public Stmt {
  enum { COND, BODY, END_EXPR };
  Stmt* SubExprs[END_EXPR];
  SourceLocation DoLoc;
public:
  DoStmt(Stmt *body, Expr *cond, SourceLocation DL) 
    : Stmt(DoStmtClass), DoLoc(DL) {
    SubExprs[COND] = reinterpret_cast<Stmt*>(cond);
    SubExprs[BODY] = body;
    DoLoc = DL;
  }  
  
  Expr *getCond() { return reinterpret_cast<Expr*>(SubExprs[COND]); }
  const Expr *getCond() const { return reinterpret_cast<Expr*>(SubExprs[COND]);}
  Stmt *getBody() { return SubExprs[BODY]; }
  const Stmt *getBody() const { return SubExprs[BODY]; }  

  virtual SourceRange getSourceRange() const { 
    return SourceRange(DoLoc, SubExprs[BODY]->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == DoStmtClass; 
  }
  static bool classof(const DoStmt *) { return true; }

  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static DoStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};


/// ForStmt - This represents a 'for (init;cond;inc)' stmt.  Note that any of
/// the init/cond/inc parts of the ForStmt will be null if they were not
/// specified in the source.
///
class ForStmt : public Stmt {
  enum { INIT, COND, INC, BODY, END_EXPR };
  Stmt* SubExprs[END_EXPR]; // SubExprs[INIT] is an expression or declstmt.
  SourceLocation ForLoc;
public:
  ForStmt(Stmt *Init, Expr *Cond, Expr *Inc, Stmt *Body, SourceLocation FL) 
    : Stmt(ForStmtClass) {
    SubExprs[INIT] = Init;
    SubExprs[COND] = reinterpret_cast<Stmt*>(Cond);
    SubExprs[INC] = reinterpret_cast<Stmt*>(Inc);
    SubExprs[BODY] = Body;
    ForLoc = FL;
  }
  
  Stmt *getInit() { return SubExprs[INIT]; }
  Expr *getCond() { return reinterpret_cast<Expr*>(SubExprs[COND]); }
  Expr *getInc()  { return reinterpret_cast<Expr*>(SubExprs[INC]); }
  Stmt *getBody() { return SubExprs[BODY]; }

  const Stmt *getInit() const { return SubExprs[INIT]; }
  const Expr *getCond() const { return reinterpret_cast<Expr*>(SubExprs[COND]);}
  const Expr *getInc()  const { return reinterpret_cast<Expr*>(SubExprs[INC]); }
  const Stmt *getBody() const { return SubExprs[BODY]; }

  virtual SourceRange getSourceRange() const { 
    return SourceRange(ForLoc, SubExprs[BODY]->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == ForStmtClass; 
  }
  static bool classof(const ForStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ForStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};
  
/// GotoStmt - This represents a direct goto.
///
class GotoStmt : public Stmt {
  LabelStmt *Label;
  SourceLocation GotoLoc;
  SourceLocation LabelLoc;
public:
  GotoStmt(LabelStmt *label, SourceLocation GL, SourceLocation LL) 
    : Stmt(GotoStmtClass), Label(label), GotoLoc(GL), LabelLoc(LL) {}
  
  LabelStmt *getLabel() const { return Label; }

  virtual SourceRange getSourceRange() const { 
    return SourceRange(GotoLoc, LabelLoc); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == GotoStmtClass; 
  }
  static bool classof(const GotoStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static GotoStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// IndirectGotoStmt - This represents an indirect goto.
///
class IndirectGotoStmt : public Stmt {
  Stmt *Target;
  // FIXME: Add location information (e.g. SourceLocation objects).
  //        When doing so, update the serialization routines.
public:
  IndirectGotoStmt(Expr *target) : Stmt(IndirectGotoStmtClass),
                                   Target((Stmt*)target){}
  
  Expr *getTarget();
  const Expr *getTarget() const;

  virtual SourceRange getSourceRange() const { return SourceRange(); }
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == IndirectGotoStmtClass; 
  }
  static bool classof(const IndirectGotoStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static IndirectGotoStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};


/// ContinueStmt - This represents a continue.
///
class ContinueStmt : public Stmt {
  SourceLocation ContinueLoc;
public:
  ContinueStmt(SourceLocation CL) : Stmt(ContinueStmtClass), ContinueLoc(CL) {}
  
  virtual SourceRange getSourceRange() const { 
    return SourceRange(ContinueLoc); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == ContinueStmtClass; 
  }
  static bool classof(const ContinueStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ContinueStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// BreakStmt - This represents a break.
///
class BreakStmt : public Stmt {
  SourceLocation BreakLoc;
public:
  BreakStmt(SourceLocation BL) : Stmt(BreakStmtClass), BreakLoc(BL) {}
  
  virtual SourceRange getSourceRange() const { return SourceRange(BreakLoc); }

  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == BreakStmtClass; 
  }
  static bool classof(const BreakStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static BreakStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};


/// ReturnStmt - This represents a return, optionally of an expression:
///   return;
///   return 4;
///
/// Note that GCC allows return with no argument in a function declared to
/// return a value, and it allows returning a value in functions declared to
/// return void.  We explicitly model this in the AST, which means you can't
/// depend on the return type of the function and the presence of an argument.
///
class ReturnStmt : public Stmt {
  Stmt *RetExpr;
  SourceLocation RetLoc;
public:
  ReturnStmt(SourceLocation RL, Expr *E = 0) : Stmt(ReturnStmtClass), 
    RetExpr((Stmt*) E), RetLoc(RL) {}
  
  const Expr *getRetValue() const;
  Expr *getRetValue();

  virtual SourceRange getSourceRange() const;
  
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == ReturnStmtClass; 
  }
  static bool classof(const ReturnStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ReturnStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// AsmStmt - This represents a GNU inline-assembly statement extension.
///
class AsmStmt : public Stmt {
  SourceLocation AsmLoc, RParenLoc;
  StringLiteral *AsmStr;

  bool IsSimple;
  bool IsVolatile;
  
  unsigned NumOutputs;
  unsigned NumInputs;
  
  llvm::SmallVector<std::string, 4> Names;
  llvm::SmallVector<StringLiteral*, 4> Constraints;
  llvm::SmallVector<Stmt*, 4> Exprs;

  llvm::SmallVector<StringLiteral*, 4> Clobbers;
public:
  AsmStmt(SourceLocation asmloc, bool issimple, bool isvolatile, 
          unsigned numoutputs, unsigned numinputs, 
          std::string *names, StringLiteral **constraints,
          Expr **exprs, StringLiteral *asmstr, unsigned numclobbers,
          StringLiteral **clobbers, SourceLocation rparenloc);

  bool isVolatile() const { return IsVolatile; }
  bool isSimple() const { return IsSimple; }

  //===--- Asm String Analysis ---===//

  const StringLiteral *getAsmString() const { return AsmStr; }
  StringLiteral *getAsmString() { return AsmStr; }
  
  /// AsmStringPiece - this is part of a decomposed asm string specification
  /// (for use with the AnalyzeAsmString function below).  An asm string is
  /// considered to be a concatenation of these parts.
  class AsmStringPiece {
  public:
    enum Kind {
      String,  // String in .ll asm string form, "$" -> "$$" and "%%" -> "%".
      Operand  // Operand reference, with optional modifier %c4.
    };
  private:
    Kind MyKind;
    std::string Str;
    unsigned OperandNo;
  public:
    AsmStringPiece(const std::string &S) : MyKind(String), Str(S) {}
    AsmStringPiece(unsigned OpNo, char Modifier)
      : MyKind(Operand), Str(), OperandNo(OpNo) {
      Str += Modifier;
    }
    
    bool isString() const { return MyKind == String; }
    bool isOperand() const { return MyKind == Operand; }
    
    const std::string &getString() const {
      assert(isString());
      return Str;
    }

    unsigned getOperandNo() const {
      assert(isOperand());
      return OperandNo;
    }
    
    /// getModifier - Get the modifier for this operand, if present.  This
    /// returns '\0' if there was no modifier.
    char getModifier() const {
      assert(isOperand());
      return Str[0];
    }
  };
  
  /// AnalyzeAsmString - Analyze the asm string of the current asm, decomposing
  /// it into pieces.  If the asm string is erroneous, emit errors and return
  /// true, otherwise return false.  This handles canonicalization and
  /// translation of strings from GCC syntax to LLVM IR syntax, and handles
  //// flattening of named references like %[foo] to Operand AsmStringPiece's. 
  unsigned AnalyzeAsmString(llvm::SmallVectorImpl<AsmStringPiece> &Pieces,
                            ASTContext &C, unsigned &DiagOffs) const;
  
  
  //===--- Output operands ---===//

  unsigned getNumOutputs() const { return NumOutputs; }

  const std::string &getOutputName(unsigned i) const {
    return Names[i];
  }

  /// getOutputConstraint - Return the constraint string for the specified
  /// output operand.  All output constraints are known to be non-empty (either
  /// '=' or '+').
  std::string getOutputConstraint(unsigned i) const;
  
  const StringLiteral *getOutputConstraintLiteral(unsigned i) const {
    return Constraints[i];
  }
  StringLiteral *getOutputConstraintLiteral(unsigned i) {
    return Constraints[i];
  }
  
  
  Expr *getOutputExpr(unsigned i);
  
  const Expr *getOutputExpr(unsigned i) const {
    return const_cast<AsmStmt*>(this)->getOutputExpr(i);
  }
  
  /// isOutputPlusConstraint - Return true if the specified output constraint
  /// is a "+" constraint (which is both an input and an output) or false if it
  /// is an "=" constraint (just an output).
  bool isOutputPlusConstraint(unsigned i) const {
    return getOutputConstraint(i)[0] == '+';
  }
  
  /// getNumPlusOperands - Return the number of output operands that have a "+"
  /// constraint.
  unsigned getNumPlusOperands() const;
  
  //===--- Input operands ---===//
  
  unsigned getNumInputs() const { return NumInputs; }  
  
  const std::string &getInputName(unsigned i) const {
    return Names[i + NumOutputs];
  }
  
  /// getInputConstraint - Return the specified input constraint.  Unlike output
  /// constraints, these can be empty.
  std::string getInputConstraint(unsigned i) const;
  
  const StringLiteral *getInputConstraintLiteral(unsigned i) const {
    return Constraints[i + NumOutputs];
  }
  StringLiteral *getInputConstraintLiteral(unsigned i) {
    return Constraints[i + NumOutputs];
  }
  
  
  Expr *getInputExpr(unsigned i);
  
  const Expr *getInputExpr(unsigned i) const {
    return const_cast<AsmStmt*>(this)->getInputExpr(i);
  }
  
  //===--- Other ---===//
  
  /// getNamedOperand - Given a symbolic operand reference like %[foo],
  /// translate this into a numeric value needed to reference the same operand.
  /// This returns -1 if the operand name is invalid.
  int getNamedOperand(const std::string &SymbolicName) const;

  

  unsigned getNumClobbers() const { return Clobbers.size(); }
  StringLiteral *getClobber(unsigned i) { return Clobbers[i]; }
  const StringLiteral *getClobber(unsigned i) const { return Clobbers[i]; }
  
  virtual SourceRange getSourceRange() const {
    return SourceRange(AsmLoc, RParenLoc);
  }
  
  static bool classof(const Stmt *T) {return T->getStmtClass() == AsmStmtClass;}
  static bool classof(const AsmStmt *) { return true; }
  
  // Input expr iterators.
  
  typedef ExprIterator inputs_iterator;
  typedef ConstExprIterator const_inputs_iterator;
  
  inputs_iterator begin_inputs() {
    return &Exprs[0] + NumOutputs;
  }
  
  inputs_iterator end_inputs() {
    return  &Exprs[0] + NumOutputs + NumInputs;
  }
  
  const_inputs_iterator begin_inputs() const {
    return &Exprs[0] + NumOutputs;
  }
  
  const_inputs_iterator end_inputs() const {
    return  &Exprs[0] + NumOutputs + NumInputs;}
  
  // Output expr iterators.
  
  typedef ExprIterator outputs_iterator;
  typedef ConstExprIterator const_outputs_iterator;
  
  outputs_iterator begin_outputs() { return &Exprs[0]; }
  outputs_iterator end_outputs() { return &Exprs[0] + NumOutputs; }
  
  const_outputs_iterator begin_outputs() const { return &Exprs[0]; }
  const_outputs_iterator end_outputs() const { return &Exprs[0] + NumOutputs; }
  
  // Input name iterator.
  
  const std::string *begin_output_names() const {
    return &Names[0];
  }
  
  const std::string *end_output_names() const {
    return &Names[0] + NumOutputs;
  }
  
  // Child iterators  
  
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static AsmStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// ObjCForCollectionStmt - This represents Objective-c's collection statement;
/// represented as 'for (element 'in' collection-expression)' stmt.
///
class ObjCForCollectionStmt : public Stmt {
  enum { ELEM, COLLECTION, BODY, END_EXPR };
  Stmt* SubExprs[END_EXPR]; // SubExprs[ELEM] is an expression or declstmt.
  SourceLocation ForLoc;
  SourceLocation RParenLoc;
public:
  ObjCForCollectionStmt(Stmt *Elem, Expr *Collect, Stmt *Body, 
                        SourceLocation FCL, SourceLocation RPL);
  
  Stmt *getElement() { return SubExprs[ELEM]; }
  Expr *getCollection() { 
    return reinterpret_cast<Expr*>(SubExprs[COLLECTION]); 
  }
  Stmt *getBody() { return SubExprs[BODY]; }
  
  const Stmt *getElement() const { return SubExprs[ELEM]; }
  const Expr *getCollection() const { 
    return reinterpret_cast<Expr*>(SubExprs[COLLECTION]);
  }
  const Stmt *getBody() const { return SubExprs[BODY]; }
  
  SourceLocation getRParenLoc() const { return RParenLoc; }
  
  virtual SourceRange getSourceRange() const { 
    return SourceRange(ForLoc, SubExprs[BODY]->getLocEnd()); 
  }
  static bool classof(const Stmt *T) { 
    return T->getStmtClass() == ObjCForCollectionStmtClass; 
  }
  static bool classof(const ObjCForCollectionStmt *) { return true; }
  
  // Iterators
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ObjCForCollectionStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};  
  
/// ObjCAtCatchStmt - This represents objective-c's @catch statement.
class ObjCAtCatchStmt : public Stmt {
private:
  enum { BODY, NEXT_CATCH, END_EXPR };
  ParmVarDecl *ExceptionDecl;
  Stmt *SubExprs[END_EXPR];
  SourceLocation AtCatchLoc, RParenLoc;

  // Used by deserialization.
  ObjCAtCatchStmt(SourceLocation atCatchLoc, SourceLocation rparenloc)
  : Stmt(ObjCAtCatchStmtClass), AtCatchLoc(atCatchLoc), RParenLoc(rparenloc) {}

public:
  ObjCAtCatchStmt(SourceLocation atCatchLoc, SourceLocation rparenloc,
                  ParmVarDecl *catchVarDecl, 
                  Stmt *atCatchStmt, Stmt *atCatchList);
  
  const Stmt *getCatchBody() const { return SubExprs[BODY]; }
  Stmt *getCatchBody() { return SubExprs[BODY]; }

  const ObjCAtCatchStmt *getNextCatchStmt() const {
    return static_cast<const ObjCAtCatchStmt*>(SubExprs[NEXT_CATCH]);
  }
  ObjCAtCatchStmt *getNextCatchStmt() { 
    return static_cast<ObjCAtCatchStmt*>(SubExprs[NEXT_CATCH]);
  }

  const ParmVarDecl *getCatchParamDecl() const { 
    return ExceptionDecl; 
  }
  ParmVarDecl *getCatchParamDecl() { 
    return ExceptionDecl; 
  }
  
  SourceLocation getRParenLoc() const { return RParenLoc; }
  
  virtual SourceRange getSourceRange() const { 
    return SourceRange(AtCatchLoc, SubExprs[BODY]->getLocEnd()); 
  }

  bool hasEllipsis() const { return getCatchParamDecl() == 0; }
  
  static bool classof(const Stmt *T) {
    return T->getStmtClass() == ObjCAtCatchStmtClass;
  }
  static bool classof(const ObjCAtCatchStmt *) { return true; }
  
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ObjCAtCatchStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};
  
/// ObjCAtFinallyStmt - This represent objective-c's @finally Statement 
class ObjCAtFinallyStmt : public Stmt {
  Stmt *AtFinallyStmt;
  SourceLocation AtFinallyLoc;    
public:
  ObjCAtFinallyStmt(SourceLocation atFinallyLoc, Stmt *atFinallyStmt)
  : Stmt(ObjCAtFinallyStmtClass), 
    AtFinallyStmt(atFinallyStmt), AtFinallyLoc(atFinallyLoc) {}
  
  const Stmt *getFinallyBody () const { return AtFinallyStmt; }
  Stmt *getFinallyBody () { return AtFinallyStmt; }

  virtual SourceRange getSourceRange() const { 
    return SourceRange(AtFinallyLoc, AtFinallyStmt->getLocEnd()); 
  }
  
  static bool classof(const Stmt *T) {
    return T->getStmtClass() == ObjCAtFinallyStmtClass;
  }
  static bool classof(const ObjCAtFinallyStmt *) { return true; }
  
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ObjCAtFinallyStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};
  
/// ObjCAtTryStmt - This represent objective-c's over-all 
/// @try ... @catch ... @finally statement.
class ObjCAtTryStmt : public Stmt {
private:
  enum { TRY, CATCH, FINALLY, END_EXPR };
  Stmt* SubStmts[END_EXPR]; 
  
  SourceLocation AtTryLoc;      
public:
  ObjCAtTryStmt(SourceLocation atTryLoc, Stmt *atTryStmt, 
                Stmt *atCatchStmt, 
                Stmt *atFinallyStmt)
  : Stmt(ObjCAtTryStmtClass) {
      SubStmts[TRY] = atTryStmt;
      SubStmts[CATCH] = atCatchStmt;
      SubStmts[FINALLY] = atFinallyStmt;
      AtTryLoc = atTryLoc;
    }
    
  const Stmt *getTryBody() const { return SubStmts[TRY]; }
  Stmt *getTryBody() { return SubStmts[TRY]; }
  const ObjCAtCatchStmt *getCatchStmts() const { 
    return dyn_cast_or_null<ObjCAtCatchStmt>(SubStmts[CATCH]); 
  }
  ObjCAtCatchStmt *getCatchStmts() { 
    return dyn_cast_or_null<ObjCAtCatchStmt>(SubStmts[CATCH]); 
  }
  const ObjCAtFinallyStmt *getFinallyStmt() const { 
    return dyn_cast_or_null<ObjCAtFinallyStmt>(SubStmts[FINALLY]); 
  }
  ObjCAtFinallyStmt *getFinallyStmt() { 
    return dyn_cast_or_null<ObjCAtFinallyStmt>(SubStmts[FINALLY]); 
  }
  virtual SourceRange getSourceRange() const { 
    return SourceRange(AtTryLoc, SubStmts[TRY]->getLocEnd()); 
  }
    
  static bool classof(const Stmt *T) {
    return T->getStmtClass() == ObjCAtTryStmtClass;
  }
  static bool classof(const ObjCAtTryStmt *) { return true; }
    
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ObjCAtTryStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// ObjCAtSynchronizedStmt - This is for objective-c's @synchronized statement.
/// Example: @synchronized (sem) {
///             do-something;
///          }
///
class ObjCAtSynchronizedStmt : public Stmt {
private:
  enum { SYNC_EXPR, SYNC_BODY, END_EXPR };
  Stmt* SubStmts[END_EXPR];
  SourceLocation AtSynchronizedLoc;
  
public:
  ObjCAtSynchronizedStmt(SourceLocation atSynchronizedLoc, Stmt *synchExpr,
                         Stmt *synchBody)
  : Stmt(ObjCAtSynchronizedStmtClass) {
      SubStmts[SYNC_EXPR] = synchExpr;
      SubStmts[SYNC_BODY] = synchBody;
      AtSynchronizedLoc = atSynchronizedLoc;
    }
  
  const CompoundStmt *getSynchBody() const {
    return reinterpret_cast<CompoundStmt*>(SubStmts[SYNC_BODY]);
  }
  CompoundStmt *getSynchBody() { 
    return reinterpret_cast<CompoundStmt*>(SubStmts[SYNC_BODY]); 
  }
  
  const Expr *getSynchExpr() const { 
    return reinterpret_cast<Expr*>(SubStmts[SYNC_EXPR]); 
  }
  Expr *getSynchExpr() { 
    return reinterpret_cast<Expr*>(SubStmts[SYNC_EXPR]); 
  }
  
  virtual SourceRange getSourceRange() const { 
    return SourceRange(AtSynchronizedLoc, getSynchBody()->getLocEnd()); 
  }
  
  static bool classof(const Stmt *T) {
    return T->getStmtClass() == ObjCAtSynchronizedStmtClass;
  }
  static bool classof(const ObjCAtSynchronizedStmt *) { return true; }
  
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ObjCAtSynchronizedStmt* CreateImpl(llvm::Deserializer& D,
                                            ASTContext& C);
};
  
/// ObjCAtThrowStmt - This represents objective-c's @throw statement.
class ObjCAtThrowStmt : public Stmt {
  Stmt *Throw;
  SourceLocation AtThrowLoc;
public:
  ObjCAtThrowStmt(SourceLocation atThrowLoc, Stmt *throwExpr)
  : Stmt(ObjCAtThrowStmtClass), Throw(throwExpr) {
    AtThrowLoc = atThrowLoc;
  }
  
  const Expr *getThrowExpr() const { return reinterpret_cast<Expr*>(Throw); }
  Expr *getThrowExpr() { return reinterpret_cast<Expr*>(Throw); }
  
  virtual SourceRange getSourceRange() const {
    if (Throw)
      return SourceRange(AtThrowLoc, Throw->getLocEnd()); 
    else 
      return SourceRange(AtThrowLoc);
  }
  
  static bool classof(const Stmt *T) {
    return T->getStmtClass() == ObjCAtThrowStmtClass;
  }
  static bool classof(const ObjCAtThrowStmt *) { return true; }
  
  virtual child_iterator child_begin();
  virtual child_iterator child_end();
  
  virtual void EmitImpl(llvm::Serializer& S) const;
  static ObjCAtThrowStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// CXXCatchStmt - This represents a C++ catch block.
class CXXCatchStmt : public Stmt {
  SourceLocation CatchLoc;
  /// The exception-declaration of the type.
  Decl *ExceptionDecl;
  /// The handler block.
  Stmt *HandlerBlock;

public:
  CXXCatchStmt(SourceLocation catchLoc, Decl *exDecl, Stmt *handlerBlock)
  : Stmt(CXXCatchStmtClass), CatchLoc(catchLoc), ExceptionDecl(exDecl),
    HandlerBlock(handlerBlock) {}

  virtual void Destroy(ASTContext& Ctx);

  virtual SourceRange getSourceRange() const {
    return SourceRange(CatchLoc, HandlerBlock->getLocEnd());
  }

  Decl *getExceptionDecl() { return ExceptionDecl; }
  QualType getCaughtType();
  Stmt *getHandlerBlock() { return HandlerBlock; }

  static bool classof(const Stmt *T) {
    return T->getStmtClass() == CXXCatchStmtClass;
  }
  static bool classof(const CXXCatchStmt *) { return true; }

  virtual child_iterator child_begin();
  virtual child_iterator child_end();

  virtual void EmitImpl(llvm::Serializer& S) const;
  static CXXCatchStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

/// CXXTryStmt - A C++ try block, including all handlers.
class CXXTryStmt : public Stmt {
  SourceLocation TryLoc;
  // First place is the guarded CompoundStatement. Subsequent are the handlers.
  // More than three handlers should be rare.
  llvm::SmallVector<Stmt*, 4> Stmts;

public:
  CXXTryStmt(SourceLocation tryLoc, Stmt *tryBlock,
             Stmt **handlers, unsigned numHandlers);

  virtual SourceRange getSourceRange() const {
    return SourceRange(TryLoc, Stmts.back()->getLocEnd());
  }

  CompoundStmt *getTryBlock() { return llvm::cast<CompoundStmt>(Stmts[0]); }
  const CompoundStmt *getTryBlock() const {
    return llvm::cast<CompoundStmt>(Stmts[0]);
  }

  unsigned getNumHandlers() const { return Stmts.size() - 1; }
  CXXCatchStmt *getHandler(unsigned i) {
    return llvm::cast<CXXCatchStmt>(Stmts[i + 1]);
  }
  const CXXCatchStmt *getHandler(unsigned i) const {
    return llvm::cast<CXXCatchStmt>(Stmts[i + 1]);
  }

  static bool classof(const Stmt *T) {
    return T->getStmtClass() == CXXTryStmtClass;
  }
  static bool classof(const CXXTryStmt *) { return true; }

  virtual child_iterator child_begin();
  virtual child_iterator child_end();

  virtual void EmitImpl(llvm::Serializer& S) const;
  static CXXTryStmt* CreateImpl(llvm::Deserializer& D, ASTContext& C);
};

}  // end namespace clang

#endif
