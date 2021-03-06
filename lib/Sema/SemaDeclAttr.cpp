//===--- SemaDeclAttr.cpp - Declaration Attribute Handling ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements decl-related attribute processing.
//
//===----------------------------------------------------------------------===//

#include "Sema.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Parse/DeclSpec.h"
#include <llvm/ADT/StringExtras.h>
using namespace clang;

//===----------------------------------------------------------------------===//
//  Helper functions
//===----------------------------------------------------------------------===//

static const FunctionType *getFunctionType(Decl *d) {
  QualType Ty;
  if (ValueDecl *decl = dyn_cast<ValueDecl>(d))
    Ty = decl->getType();
  else if (FieldDecl *decl = dyn_cast<FieldDecl>(d))
    Ty = decl->getType();
  else if (TypedefDecl* decl = dyn_cast<TypedefDecl>(d))
    Ty = decl->getUnderlyingType();
  else
    return 0;
  
  if (Ty->isFunctionPointerType())
    Ty = Ty->getAsPointerType()->getPointeeType();

  return Ty->getAsFunctionType();
}

// FIXME: We should provide an abstraction around a method or function
// to provide the following bits of information.

/// isFunctionOrMethod - Return true if the given decl has function
/// type (function or function-typed variable) or an Objective-C
/// method.
static bool isFunctionOrMethod(Decl *d) {
  return getFunctionType(d) || isa<ObjCMethodDecl>(d);
}

/// hasFunctionProto - Return true if the given decl has a argument
/// information. This decl should have already passed
/// isFunctionOrMethod.
static bool hasFunctionProto(Decl *d) {
  if (const FunctionType *FnTy = getFunctionType(d)) {
    return isa<FunctionProtoType>(FnTy);
  } else {
    assert(isa<ObjCMethodDecl>(d));
    return true;
  }
}

/// getFunctionOrMethodNumArgs - Return number of function or method
/// arguments. It is an error to call this on a K&R function (use
/// hasFunctionProto first).
static unsigned getFunctionOrMethodNumArgs(Decl *d) {
  if (const FunctionType *FnTy = getFunctionType(d))
    return cast<FunctionProtoType>(FnTy)->getNumArgs();
  return cast<ObjCMethodDecl>(d)->param_size();
}

static QualType getFunctionOrMethodArgType(Decl *d, unsigned Idx) {
  if (const FunctionType *FnTy = getFunctionType(d))
    return cast<FunctionProtoType>(FnTy)->getArgType(Idx);
  
  return cast<ObjCMethodDecl>(d)->param_begin()[Idx]->getType();
}

static bool isFunctionOrMethodVariadic(Decl *d) {
  if (const FunctionType *FnTy = getFunctionType(d)) {
    const FunctionProtoType *proto = cast<FunctionProtoType>(FnTy);
    return proto->isVariadic();
  } else {
    return cast<ObjCMethodDecl>(d)->isVariadic();
  }
}

static inline bool isNSStringType(QualType T, ASTContext &Ctx) {
  const PointerType *PT = T->getAsPointerType();
  if (!PT)
    return false;
  
  const ObjCInterfaceType *ClsT =PT->getPointeeType()->getAsObjCInterfaceType();
  if (!ClsT)
    return false;
  
  IdentifierInfo* ClsName = ClsT->getDecl()->getIdentifier();
  
  // FIXME: Should we walk the chain of classes?
  return ClsName == &Ctx.Idents.get("NSString") ||
         ClsName == &Ctx.Idents.get("NSMutableString");
}

static inline bool isCFStringType(QualType T, ASTContext &Ctx) {
  const PointerType *PT = T->getAsPointerType();
  if (!PT)
    return false;

  const RecordType *RT = PT->getPointeeType()->getAsRecordType();
  if (!RT)
    return false;
  
  const RecordDecl *RD = RT->getDecl();
  if (RD->getTagKind() != TagDecl::TK_struct)
    return false;

  return RD->getIdentifier() == &Ctx.Idents.get("__CFString");
}

//===----------------------------------------------------------------------===//
// Attribute Implementations
//===----------------------------------------------------------------------===//

// FIXME: All this manual attribute parsing code is gross. At the
// least add some helper functions to check most argument patterns (#
// and types of args).

static void HandleExtVectorTypeAttr(Decl *d, const AttributeList &Attr,
                                    Sema &S) {
  TypedefDecl *tDecl = dyn_cast<TypedefDecl>(d);
  if (tDecl == 0) {
    S.Diag(Attr.getLoc(), diag::err_typecheck_ext_vector_not_typedef);
    return;
  }
  
  QualType curType = tDecl->getUnderlyingType();
  // check the attribute arguments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  Expr *sizeExpr = static_cast<Expr *>(Attr.getArg(0));
  llvm::APSInt vecSize(32);
  if (!sizeExpr->isIntegerConstantExpr(vecSize, S.Context)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_not_int)
      << "ext_vector_type" << sizeExpr->getSourceRange();
    return;
  }
  // unlike gcc's vector_size attribute, we do not allow vectors to be defined
  // in conjunction with complex types (pointers, arrays, functions, etc.).
  if (!curType->isIntegerType() && !curType->isRealFloatingType()) {
    S.Diag(Attr.getLoc(), diag::err_attribute_invalid_vector_type) << curType;
    return;
  }
  // unlike gcc's vector_size attribute, the size is specified as the 
  // number of elements, not the number of bytes.
  unsigned vectorSize = static_cast<unsigned>(vecSize.getZExtValue()); 
  
  if (vectorSize == 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_zero_size)
      << sizeExpr->getSourceRange();
    return;
  }
  // Instantiate/Install the vector type, the number of elements is > 0.
  tDecl->setUnderlyingType(S.Context.getExtVectorType(curType, vectorSize));
  // Remember this typedef decl, we will need it later for diagnostics.
  S.ExtVectorDecls.push_back(tDecl);
}


/// HandleVectorSizeAttribute - this attribute is only applicable to 
/// integral and float scalars, although arrays, pointers, and function
/// return values are allowed in conjunction with this construct. Aggregates
/// with this attribute are invalid, even if they are of the same size as a
/// corresponding scalar.
/// The raw attribute should contain precisely 1 argument, the vector size 
/// for the variable, measured in bytes. If curType and rawAttr are well
/// formed, this routine will return a new vector type.
static void HandleVectorSizeAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  QualType CurType;
  if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
    CurType = VD->getType();
  else if (TypedefDecl *TD = dyn_cast<TypedefDecl>(D))
    CurType = TD->getUnderlyingType();
  else {
    S.Diag(D->getLocation(), diag::err_attr_wrong_decl)
      << "vector_size" << SourceRange(Attr.getLoc(), Attr.getLoc());
    return;
  }
    
  // Check the attribute arugments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  Expr *sizeExpr = static_cast<Expr *>(Attr.getArg(0));
  llvm::APSInt vecSize(32);
  if (!sizeExpr->isIntegerConstantExpr(vecSize, S.Context)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_not_int)
      << "vector_size" << sizeExpr->getSourceRange();
    return;
  }
  // navigate to the base type - we need to provide for vector pointers, 
  // vector arrays, and functions returning vectors.
  if (CurType->isPointerType() || CurType->isArrayType() ||
      CurType->isFunctionType()) {
    assert(0 && "HandleVector(): Complex type construction unimplemented");
    /* FIXME: rebuild the type from the inside out, vectorizing the inner type.
     do {
     if (PointerType *PT = dyn_cast<PointerType>(canonType))
     canonType = PT->getPointeeType().getTypePtr();
     else if (ArrayType *AT = dyn_cast<ArrayType>(canonType))
     canonType = AT->getElementType().getTypePtr();
     else if (FunctionType *FT = dyn_cast<FunctionType>(canonType))
     canonType = FT->getResultType().getTypePtr();
     } while (canonType->isPointerType() || canonType->isArrayType() ||
     canonType->isFunctionType());
     */
  }
  // the base type must be integer or float.
  if (!CurType->isIntegerType() && !CurType->isRealFloatingType()) {
    S.Diag(Attr.getLoc(), diag::err_attribute_invalid_vector_type) << CurType;
    return;
  }
  unsigned typeSize = static_cast<unsigned>(S.Context.getTypeSize(CurType));
  // vecSize is specified in bytes - convert to bits.
  unsigned vectorSize = static_cast<unsigned>(vecSize.getZExtValue() * 8); 
  
  // the vector size needs to be an integral multiple of the type size.
  if (vectorSize % typeSize) {
    S.Diag(Attr.getLoc(), diag::err_attribute_invalid_size)
      << sizeExpr->getSourceRange();
    return;
  }
  if (vectorSize == 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_zero_size)
      << sizeExpr->getSourceRange();
    return;
  }
  
  // Success! Instantiate the vector type, the number of elements is > 0, and
  // not required to be a power of 2, unlike GCC.
  CurType = S.Context.getVectorType(CurType, vectorSize/typeSize);
  
  if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
    VD->setType(CurType);
  else 
    cast<TypedefDecl>(D)->setUnderlyingType(CurType);
}

static void HandlePackedAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() > 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  if (TagDecl *TD = dyn_cast<TagDecl>(d))
    TD->addAttr(::new (S.Context) PackedAttr(1));
  else if (FieldDecl *FD = dyn_cast<FieldDecl>(d)) {
    // If the alignment is less than or equal to 8 bits, the packed attribute
    // has no effect.
    if (!FD->getType()->isIncompleteType() &&
        S.Context.getTypeAlign(FD->getType()) <= 8)
      S.Diag(Attr.getLoc(), diag::warn_attribute_ignored_for_field_of_type)
        << Attr.getName() << FD->getType();
    else
      FD->addAttr(::new (S.Context) PackedAttr(1));
  } else
    S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << Attr.getName();
}

static void HandleIBOutletAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() > 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  // The IBOutlet attribute only applies to instance variables of Objective-C
  // classes.
  if (isa<ObjCIvarDecl>(d) || isa<ObjCPropertyDecl>(d))
    d->addAttr(::new (S.Context) IBOutletAttr());
  else
    S.Diag(Attr.getLoc(), diag::err_attribute_iboutlet);
}

static void HandleNonNullAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // GCC ignores the nonnull attribute on K&R style function
  // prototypes, so we ignore it as well
  if (!isFunctionOrMethod(d) || !hasFunctionProto(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "nonnull" << 0 /*function*/;
    return;
  }
  
  unsigned NumArgs = getFunctionOrMethodNumArgs(d);

  // The nonnull attribute only applies to pointers.
  llvm::SmallVector<unsigned, 10> NonNullArgs;
  
  for (AttributeList::arg_iterator I=Attr.arg_begin(),
                                   E=Attr.arg_end(); I!=E; ++I) {
    
    
    // The argument must be an integer constant expression.
    Expr *Ex = static_cast<Expr *>(*I);
    llvm::APSInt ArgNum(32);
    if (!Ex->isIntegerConstantExpr(ArgNum, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_not_int)
        << "nonnull" << Ex->getSourceRange();
      return;
    }
    
    unsigned x = (unsigned) ArgNum.getZExtValue();
        
    if (x < 1 || x > NumArgs) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_out_of_bounds)
       << "nonnull" << I.getArgNum() << Ex->getSourceRange();
      return;
    }
    
    --x;

    // Is the function argument a pointer type?
    QualType T = getFunctionOrMethodArgType(d, x);    
    if (!T->isPointerType() && !T->isBlockPointerType()) {
      // FIXME: Should also highlight argument in decl.
      S.Diag(Attr.getLoc(), diag::err_nonnull_pointers_only)
        << "nonnull" << Ex->getSourceRange();
      continue;
    }
    
    NonNullArgs.push_back(x);
  }
  
  // If no arguments were specified to __attribute__((nonnull)) then all
  // pointer arguments have a nonnull attribute.
  if (NonNullArgs.empty()) {
    for (unsigned I = 0, E = getFunctionOrMethodNumArgs(d); I != E; ++I) {
      QualType T = getFunctionOrMethodArgType(d, I);
      if (T->isPointerType() || T->isBlockPointerType())
        NonNullArgs.push_back(I);
    }
    
    if (NonNullArgs.empty()) {
      S.Diag(Attr.getLoc(), diag::warn_attribute_nonnull_no_pointers);
      return;
    }
  }

  unsigned* start = &NonNullArgs[0];
  unsigned size = NonNullArgs.size();
  std::sort(start, start + size);
  d->addAttr(::new (S.Context) NonNullAttr(start, size));
}

static void HandleAliasAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  
  Expr *Arg = static_cast<Expr*>(Attr.getArg(0));
  Arg = Arg->IgnoreParenCasts();
  StringLiteral *Str = dyn_cast<StringLiteral>(Arg);
  
  if (Str == 0 || Str->isWide()) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_string)
      << "alias" << 1;
    return;
  }
  
  const char *Alias = Str->getStrData();
  unsigned AliasLen = Str->getByteLength();
  
  // FIXME: check if target symbol exists in current file
  
  d->addAttr(::new (S.Context) AliasAttr(std::string(Alias, AliasLen)));
}

static void HandleAlwaysInlineAttr(Decl *d, const AttributeList &Attr, 
                                   Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  if (!isa<FunctionDecl>(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
    << "always_inline" << 0 /*function*/;
    return;
  }
  
  d->addAttr(::new (S.Context) AlwaysInlineAttr());
}

static bool HandleCommonNoReturnAttr(Decl *d, const AttributeList &Attr,
                                     Sema &S, const char *attrName) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return false;
  }

  if (!isFunctionOrMethod(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << attrName << 0 /*function*/;
    return false;
  }
  
  return true;
}

static void HandleNoReturnAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  if (HandleCommonNoReturnAttr(d, Attr, S, "noreturn"))  
    d->addAttr(::new (S.Context) NoReturnAttr());
}

static void HandleAnalyzerNoReturnAttr(Decl *d, const AttributeList &Attr,
                                       Sema &S) {
  if (HandleCommonNoReturnAttr(d, Attr, S, "analyzer_noreturn"))  
    d->addAttr(::new (S.Context) AnalyzerNoReturnAttr());
}

static void HandleUnusedAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  if (!isa<VarDecl>(d) && !isFunctionOrMethod(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "unused" << 2 /*variable and function*/;
    return;
  }
  
  d->addAttr(::new (S.Context) UnusedAttr());
}

static void HandleUsedAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  if (const VarDecl *VD = dyn_cast<VarDecl>(d)) {
    if (VD->hasLocalStorage() || VD->hasExternalStorage()) {
      S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << "used";
      return;
    }
  } else if (!isFunctionOrMethod(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "used" << 2 /*variable and function*/;
    return;
  }
  
  d->addAttr(::new (S.Context) UsedAttr());
}

static void HandleConstructorAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0 && Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments)
      << "0 or 1";
    return;
  } 

  int priority = 65535; // FIXME: Do not hardcode such constants.
  if (Attr.getNumArgs() > 0) {
    Expr *E = static_cast<Expr *>(Attr.getArg(0));
    llvm::APSInt Idx(32);
    if (!E->isIntegerConstantExpr(Idx, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_int)
        << "constructor" << 1 << E->getSourceRange();
      return;
    }
    priority = Idx.getZExtValue();
  }
  
  if (!isa<FunctionDecl>(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "constructor" << 0 /*function*/;
    return;
  }

  d->addAttr(::new (S.Context) ConstructorAttr(priority));
}

static void HandleDestructorAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0 && Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments)
       << "0 or 1";
    return;
  } 

  int priority = 65535; // FIXME: Do not hardcode such constants.
  if (Attr.getNumArgs() > 0) {
    Expr *E = static_cast<Expr *>(Attr.getArg(0));
    llvm::APSInt Idx(32);
    if (!E->isIntegerConstantExpr(Idx, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_int)
        << "destructor" << 1 << E->getSourceRange();
      return;
    }
    priority = Idx.getZExtValue();
  }
  
  if (!isa<FunctionDecl>(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "destructor" << 0 /*function*/;
    return;
  }

  d->addAttr(::new (S.Context) DestructorAttr(priority));
}

static void HandleDeprecatedAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  d->addAttr(::new (S.Context) DeprecatedAttr());
}

static void HandleUnavailableAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  d->addAttr(::new (S.Context) UnavailableAttr());
}

static void HandleVisibilityAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  
  Expr *Arg = static_cast<Expr*>(Attr.getArg(0));
  Arg = Arg->IgnoreParenCasts();
  StringLiteral *Str = dyn_cast<StringLiteral>(Arg);
  
  if (Str == 0 || Str->isWide()) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_string)
      << "visibility" << 1;
    return;
  }
  
  const char *TypeStr = Str->getStrData();
  unsigned TypeLen = Str->getByteLength();
  VisibilityAttr::VisibilityTypes type;
  
  if (TypeLen == 7 && !memcmp(TypeStr, "default", 7))
    type = VisibilityAttr::DefaultVisibility;
  else if (TypeLen == 6 && !memcmp(TypeStr, "hidden", 6))
    type = VisibilityAttr::HiddenVisibility;
  else if (TypeLen == 8 && !memcmp(TypeStr, "internal", 8))
    type = VisibilityAttr::HiddenVisibility; // FIXME
  else if (TypeLen == 9 && !memcmp(TypeStr, "protected", 9))
    type = VisibilityAttr::ProtectedVisibility;
  else {
    S.Diag(Attr.getLoc(), diag::warn_attribute_unknown_visibility) << TypeStr;
    return;
  }
  
  d->addAttr(::new (S.Context) VisibilityAttr(type));
}

static void HandleObjCExceptionAttr(Decl *D, const AttributeList &Attr,
                                    Sema &S) {
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  ObjCInterfaceDecl *OCI = dyn_cast<ObjCInterfaceDecl>(D);
  if (OCI == 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_requires_objc_interface);
    return;
  }
  
  D->addAttr(::new (S.Context) ObjCExceptionAttr());
}

static void HandleObjCNSObject(Decl *D, const AttributeList &Attr, Sema &S) {
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  if (TypedefDecl *TD = dyn_cast<TypedefDecl>(D)) {
    QualType T = TD->getUnderlyingType();
    if (!T->isPointerType() ||
        !T->getAsPointerType()->getPointeeType()->isRecordType()) {
      S.Diag(TD->getLocation(), diag::err_nsobject_attribute);
      return;
    }
  }
  D->addAttr(::new (S.Context) ObjCNSObjectAttr());
}

static void 
HandleOverloadableAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }

  if (!isa<FunctionDecl>(D)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_overloadable_not_function);
    return;
  }

  D->addAttr(::new (S.Context) OverloadableAttr());
}

static void HandleBlocksAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  if (!Attr.getParameterName()) {    
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_string)
      << "blocks" << 1;
    return;
  }
  
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  
  BlocksAttr::BlocksAttrTypes type;
  if (Attr.getParameterName()->isStr("byref"))
    type = BlocksAttr::ByRef;
  else {
    S.Diag(Attr.getLoc(), diag::warn_attribute_type_not_supported)
      << "blocks" << Attr.getParameterName();
    return;
  }
  
  d->addAttr(::new (S.Context) BlocksAttr(type));
}

static void HandleSentinelAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() > 2) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments)
      << "0, 1 or 2";
    return;
  } 
  
  int sentinel = 0;
  if (Attr.getNumArgs() > 0) {
    Expr *E = static_cast<Expr *>(Attr.getArg(0));
    llvm::APSInt Idx(32);
    if (!E->isIntegerConstantExpr(Idx, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_int)
       << "sentinel" << 1 << E->getSourceRange();
      return;
    }
    sentinel = Idx.getZExtValue();
    
    if (sentinel < 0) {
      S.Diag(Attr.getLoc(), diag::err_attribute_sentinel_less_than_zero)
        << E->getSourceRange();
      return;
    }
  }

  int nullPos = 0;
  if (Attr.getNumArgs() > 1) {
    Expr *E = static_cast<Expr *>(Attr.getArg(1));
    llvm::APSInt Idx(32);
    if (!E->isIntegerConstantExpr(Idx, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_int)
        << "sentinel" << 2 << E->getSourceRange();
      return;
    }
    nullPos = Idx.getZExtValue();
    
    if (nullPos > 1 || nullPos < 0) {
      // FIXME: This error message could be improved, it would be nice
      // to say what the bounds actually are.
      S.Diag(Attr.getLoc(), diag::err_attribute_sentinel_not_zero_or_one)
        << E->getSourceRange();
      return;
    }
  }

  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(d)) {
    const FunctionType *FT = FD->getType()->getAsFunctionType();
    assert(FT && "FunctionDecl has non-function type?");
    
    if (isa<FunctionNoProtoType>(FT)) {
      S.Diag(Attr.getLoc(), diag::warn_attribute_sentinel_named_arguments);
      return;
    }
    
    if (!cast<FunctionProtoType>(FT)->isVariadic()) {
      S.Diag(Attr.getLoc(), diag::warn_attribute_sentinel_not_variadic);
      return;
    }    
  } else if (ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(d)) {
    if (!MD->isVariadic()) {
      S.Diag(Attr.getLoc(), diag::warn_attribute_sentinel_not_variadic);
      return;
    }    
  } else {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "sentinel" << 3 /*function or method*/;
    return;
  }
  
  // FIXME: Actually create the attribute.
}

static void HandleWarnUnusedResult(Decl *D, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  // TODO: could also be applied to methods?
  FunctionDecl *Fn = dyn_cast<FunctionDecl>(D);
  if (!Fn) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
    << "warn_unused_result" << 0 /*function*/;
    return;
  }
  
  Fn->addAttr(::new (S.Context) WarnUnusedResultAttr());
}

static void HandleWeakAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  // TODO: could also be applied to methods?
  if (!isa<FunctionDecl>(D) && !isa<VarDecl>(D)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
    << "weak" << 2 /*variable and function*/;
    return;
  }
  
  D->addAttr(::new (S.Context) WeakAttr());
}

static void HandleWeakImportAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }  

  // weak_import only applies to variable & function declarations.
  bool isDef = false;
  if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
    isDef = (!VD->hasExternalStorage() || VD->getInit());
  } else if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    isDef = FD->getBody();
  } else if (isa<ObjCPropertyDecl>(D)) {
    // We ignore weak import on properties
    return;
  } else {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
    << "weak_import" << 2 /*variable and function*/;
    return;
  }

  // Merge should handle any subsequent violations.
  if (isDef) {
    S.Diag(Attr.getLoc(), 
           diag::warn_attribute_weak_import_invalid_on_definition)
      << "weak_import" << 2 /*variable and function*/;
    return;
  }

  D->addAttr(::new (S.Context) WeakImportAttr());
}

static void HandleDLLImportAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  // Attribute can be applied only to functions or variables.
  if (isa<VarDecl>(D)) {
    D->addAttr(::new (S.Context) DLLImportAttr());
    return;
  }

  FunctionDecl *FD = dyn_cast<FunctionDecl>(D);
  if (!FD) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "dllimport" << 2 /*variable and function*/;
    return;
  }

  // Currently, the dllimport attribute is ignored for inlined functions.
  // Warning is emitted.
  if (FD->isInline()) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << "dllimport";
    return;
  }

  // The attribute is also overridden by a subsequent declaration as dllexport.
  // Warning is emitted.
  for (AttributeList *nextAttr = Attr.getNext(); nextAttr;
       nextAttr = nextAttr->getNext()) {
    if (nextAttr->getKind() == AttributeList::AT_dllexport) {
      S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << "dllimport";
      return;
    }
  }

  if (D->getAttr<DLLExportAttr>()) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << "dllimport";
    return;
  }

  D->addAttr(::new (S.Context) DLLImportAttr());
}

static void HandleDLLExportAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  // Attribute can be applied only to functions or variables.
  if (isa<VarDecl>(D)) {
    D->addAttr(::new (S.Context) DLLExportAttr());
    return;
  }

  FunctionDecl *FD = dyn_cast<FunctionDecl>(D);
  if (!FD) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "dllexport" << 2 /*variable and function*/;
    return;
  }

  // Currently, the dllexport attribute is ignored for inlined functions,
  // unless the -fkeep-inline-functions flag has been used. Warning is emitted;
  if (FD->isInline()) {
    // FIXME: ... unless the -fkeep-inline-functions flag has been used.
    S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << "dllexport";
    return;
  }

  D->addAttr(::new (S.Context) DLLExportAttr());
}

static void HandleSectionAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  // Attribute has no arguments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }

  // Make sure that there is a string literal as the sections's single
  // argument.
  StringLiteral *SE = 
    dyn_cast<StringLiteral>(static_cast<Expr *>(Attr.getArg(0)));
  if (!SE) {
    // FIXME
    S.Diag(Attr.getLoc(), diag::err_attribute_annotate_no_string);
    return;
  }
  D->addAttr(::new (S.Context) SectionAttr(std::string(SE->getStrData(),
                                                     SE->getByteLength())));
}

static void HandleStdCallAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // Attribute has no arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  // Attribute can be applied only to functions.
  if (!isa<FunctionDecl>(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "stdcall" << 0 /*function*/;
    return;
  }

  // stdcall and fastcall attributes are mutually incompatible.
  if (d->getAttr<FastCallAttr>()) {
    S.Diag(Attr.getLoc(), diag::err_attributes_are_not_compatible)
      << "stdcall" << "fastcall";
    return;
  }

  d->addAttr(::new (S.Context) StdCallAttr());
}

static void HandleFastCallAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // Attribute has no arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  if (!isa<FunctionDecl>(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "fastcall" << 0 /*function*/;
    return;
  }

  // stdcall and fastcall attributes are mutually incompatible.
  if (d->getAttr<StdCallAttr>()) {
    S.Diag(Attr.getLoc(), diag::err_attributes_are_not_compatible)
      << "fastcall" << "stdcall";
    return;
  }

  d->addAttr(::new (S.Context) FastCallAttr());
}

static void HandleNothrowAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  d->addAttr(::new (S.Context) NoThrowAttr());
}

static void HandleConstAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  d->addAttr(::new (S.Context) ConstAttr());
}

static void HandlePureAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  d->addAttr(::new (S.Context) PureAttr());
}

static void HandleCleanupAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // Match gcc which ignores cleanup attrs when compiling C++.
  if (S.getLangOptions().CPlusPlus)
    return;
  
  if (!Attr.getParameterName()) {    
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  
  VarDecl *VD = dyn_cast<VarDecl>(d);
  
  if (!VD || !VD->hasLocalStorage()) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << "cleanup";
    return;
  }
  
  // Look up the function
  NamedDecl *CleanupDecl = S.LookupName(S.TUScope, Attr.getParameterName(),
                                        Sema::LookupOrdinaryName);
  if (!CleanupDecl) {
    S.Diag(Attr.getLoc(), diag::err_attribute_cleanup_arg_not_found) <<
      Attr.getParameterName();
    return;
  }
  
  FunctionDecl *FD = dyn_cast<FunctionDecl>(CleanupDecl);
  if (!FD) {
    S.Diag(Attr.getLoc(), diag::err_attribute_cleanup_arg_not_function) <<
      Attr.getParameterName();
    return;
  }

  if (FD->getNumParams() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_cleanup_func_must_take_one_arg) <<
      Attr.getParameterName();
    return;
  }
  
  // We're currently more strict than GCC about what function types we accept.
  // If this ever proves to be a problem it should be easy to fix.
  QualType Ty = S.Context.getPointerType(VD->getType());
  QualType ParamTy = FD->getParamDecl(0)->getType();
  if (S.CheckAssignmentConstraints(Ty, ParamTy) != Sema::Compatible) {
    S.Diag(Attr.getLoc(), 
           diag::err_attribute_cleanup_func_arg_incompatible_type) <<
      Attr.getParameterName() << ParamTy << Ty;
    return;
  }
  
  d->addAttr(::new (S.Context) CleanupAttr(FD));
}

/// Handle __attribute__((format(type,idx,firstarg))) attributes
/// based on http://gcc.gnu.org/onlinedocs/gcc/Function-Attributes.html
static void HandleFormatAttr(Decl *d, const AttributeList &Attr, Sema &S) {

  if (!Attr.getParameterName()) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_string)
      << "format" << 1;
    return;
  }

  if (Attr.getNumArgs() != 2) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 3;
    return;
  }

  if (!isFunctionOrMethod(d) || !hasFunctionProto(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "format" << 0 /*function*/;
    return;
  }

  // FIXME: in C++ the implicit 'this' function parameter also counts.
  // this is needed in order to be compatible with GCC
  // the index must start in 1 and the limit is numargs+1
  unsigned NumArgs  = getFunctionOrMethodNumArgs(d);
  unsigned FirstIdx = 1;

  const char *Format = Attr.getParameterName()->getName();
  unsigned FormatLen = Attr.getParameterName()->getLength();

  // Normalize the argument, __foo__ becomes foo.
  if (FormatLen > 4 && Format[0] == '_' && Format[1] == '_' &&
      Format[FormatLen - 2] == '_' && Format[FormatLen - 1] == '_') {
    Format += 2;
    FormatLen -= 4;
  }

  bool Supported = false;
  bool is_NSString = false;
  bool is_strftime = false;
  bool is_CFString = false;
  
  switch (FormatLen) {
  default: break;
  case 5: Supported = !memcmp(Format, "scanf", 5); break;
  case 6: Supported = !memcmp(Format, "printf", 6); break;
  case 7: Supported = !memcmp(Format, "strfmon", 7); break;
  case 8:
    Supported = (is_strftime = !memcmp(Format, "strftime", 8)) ||
                (is_NSString = !memcmp(Format, "NSString", 8)) ||
                (is_CFString = !memcmp(Format, "CFString", 8));
    break;
  }
      
  if (!Supported) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_type_not_supported)
      << "format" << Attr.getParameterName()->getName();
    return;
  }

  // checks for the 2nd argument
  Expr *IdxExpr = static_cast<Expr *>(Attr.getArg(0));
  llvm::APSInt Idx(32);
  if (!IdxExpr->isIntegerConstantExpr(Idx, S.Context)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_int)
      << "format" << 2 << IdxExpr->getSourceRange();
    return;
  }

  if (Idx.getZExtValue() < FirstIdx || Idx.getZExtValue() > NumArgs) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_out_of_bounds)
      << "format" << 2 << IdxExpr->getSourceRange();
    return;
  }

  // FIXME: Do we need to bounds check?
  unsigned ArgIdx = Idx.getZExtValue() - 1;
  
  // make sure the format string is really a string
  QualType Ty = getFunctionOrMethodArgType(d, ArgIdx);

  if (is_CFString) {
    if (!isCFStringType(Ty, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_format_attribute_not)
        << "a CFString" << IdxExpr->getSourceRange();
      return;
    }
  } else if (is_NSString) {
    // FIXME: do we need to check if the type is NSString*?  What are
    //  the semantics?
    if (!isNSStringType(Ty, S.Context)) {
      // FIXME: Should highlight the actual expression that has the
      // wrong type.
      S.Diag(Attr.getLoc(), diag::err_format_attribute_not)
        << "an NSString" << IdxExpr->getSourceRange();
      return;
    }    
  } else if (!Ty->isPointerType() ||
             !Ty->getAsPointerType()->getPointeeType()->isCharType()) {
    // FIXME: Should highlight the actual expression that has the
    // wrong type.
    S.Diag(Attr.getLoc(), diag::err_format_attribute_not)
      << "a string type" << IdxExpr->getSourceRange();
    return;
  }

  // check the 3rd argument
  Expr *FirstArgExpr = static_cast<Expr *>(Attr.getArg(1));
  llvm::APSInt FirstArg(32);
  if (!FirstArgExpr->isIntegerConstantExpr(FirstArg, S.Context)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_not_int)
      << "format" << 3 << FirstArgExpr->getSourceRange();
    return;
  }

  // check if the function is variadic if the 3rd argument non-zero
  if (FirstArg != 0) {
    if (isFunctionOrMethodVariadic(d)) {
      ++NumArgs; // +1 for ...
    } else {
      S.Diag(d->getLocation(), diag::err_format_attribute_requires_variadic);
      return;
    }
  }

  // strftime requires FirstArg to be 0 because it doesn't read from any
  // variable the input is just the current time + the format string.
  if (is_strftime) {
    if (FirstArg != 0) {
      S.Diag(Attr.getLoc(), diag::err_format_strftime_third_parameter)
        << FirstArgExpr->getSourceRange();
      return;
    }
  // if 0 it disables parameter checking (to use with e.g. va_list)
  } else if (FirstArg != 0 && FirstArg != NumArgs) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_out_of_bounds)
      << "format" << 3 << FirstArgExpr->getSourceRange();
    return;
  }

  d->addAttr(::new (S.Context) FormatAttr(std::string(Format, FormatLen),
                            Idx.getZExtValue(), FirstArg.getZExtValue()));
}

static void HandleTransparentUnionAttr(Decl *d, const AttributeList &Attr,
                                       Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  // FIXME: This shouldn't be restricted to typedefs
  TypedefDecl *TD = dyn_cast<TypedefDecl>(d);
  if (!TD || !TD->getUnderlyingType()->isUnionType()) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "transparent_union" << 1 /*union*/;
    return;
  }

  RecordDecl* RD = TD->getUnderlyingType()->getAsUnionType()->getDecl();

  // FIXME: Should we do a check for RD->isDefinition()?

  // FIXME: This isn't supposed to be restricted to pointers, but otherwise
  // we might silently generate incorrect code; see following code
  for (RecordDecl::field_iterator Field = RD->field_begin(S.Context),
                               FieldEnd = RD->field_end(S.Context);
       Field != FieldEnd; ++Field) {
    if (!Field->getType()->isPointerType()) {
      S.Diag(Attr.getLoc(), diag::warn_transparent_union_nonpointer);
      return;
    }
  }

  // FIXME: This is a complete hack; we should be properly propagating
  // transparent_union through Sema.  That said, this is close enough to
  // correctly compile all the common cases of transparent_union without
  // errors or warnings
  QualType NewTy = S.Context.VoidPtrTy;
  NewTy.addConst();
  TD->setUnderlyingType(NewTy);
}

static void HandleAnnotateAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }
  Expr *argExpr = static_cast<Expr *>(Attr.getArg(0));
  StringLiteral *SE = dyn_cast<StringLiteral>(argExpr);
  
  // Make sure that there is a string literal as the annotation's single
  // argument.
  if (!SE) {
    S.Diag(Attr.getLoc(), diag::err_attribute_annotate_no_string);
    return;
  }
  d->addAttr(::new (S.Context) AnnotateAttr(std::string(SE->getStrData(),
                                                        SE->getByteLength())));
}

static void HandleAlignedAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() > 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }

  unsigned Align = 0;
  if (Attr.getNumArgs() == 0) {
    // FIXME: This should be the target specific maximum alignment.
    // (For now we just use 128 bits which is the maximum on X86).
    Align = 128;
    d->addAttr(::new (S.Context) AlignedAttr(Align));
    return;
  }
  
  Expr *alignmentExpr = static_cast<Expr *>(Attr.getArg(0));
  llvm::APSInt Alignment(32);
  if (!alignmentExpr->isIntegerConstantExpr(Alignment, S.Context)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_not_int)
      << "aligned" << alignmentExpr->getSourceRange();
    return;
  }
  if (!llvm::isPowerOf2_64(Alignment.getZExtValue())) {
    S.Diag(Attr.getLoc(), diag::err_attribute_aligned_not_power_of_two) 
      << alignmentExpr->getSourceRange();
    return;
  }

  d->addAttr(::new (S.Context) AlignedAttr(Alignment.getZExtValue() * 8));
}

/// HandleModeAttr - This attribute modifies the width of a decl with
/// primitive type.
///
/// Despite what would be logical, the mode attribute is a decl attribute,
/// not a type attribute: 'int ** __attribute((mode(HI))) *G;' tries to make
/// 'G' be HImode, not an intermediate pointer.
///
static void HandleModeAttr(Decl *D, const AttributeList &Attr, Sema &S) {
  // This attribute isn't documented, but glibc uses it.  It changes
  // the width of an int or unsigned int to the specified size.

  // Check that there aren't any arguments
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  IdentifierInfo *Name = Attr.getParameterName();
  if (!Name) {
    S.Diag(Attr.getLoc(), diag::err_attribute_missing_parameter_name);
    return;
  }
  const char *Str = Name->getName();
  unsigned Len = Name->getLength();

  // Normalize the attribute name, __foo__ becomes foo.
  if (Len > 4 && Str[0] == '_' && Str[1] == '_' &&
      Str[Len - 2] == '_' && Str[Len - 1] == '_') {
    Str += 2;
    Len -= 4;
  }

  unsigned DestWidth = 0;
  bool IntegerMode = true;
  bool ComplexMode = false;
  switch (Len) {
  case 2:
    switch (Str[0]) {
    case 'Q': DestWidth = 8; break;
    case 'H': DestWidth = 16; break;
    case 'S': DestWidth = 32; break;
    case 'D': DestWidth = 64; break;
    case 'X': DestWidth = 96; break;
    case 'T': DestWidth = 128; break;
    }
    if (Str[1] == 'F') {
      IntegerMode = false;
    } else if (Str[1] == 'C') {
      IntegerMode = false;
      ComplexMode = true;
    } else if (Str[1] != 'I') {
      DestWidth = 0;
    }
    break;
  case 4:
    // FIXME: glibc uses 'word' to define register_t; this is narrower than a
    // pointer on PIC16 and other embedded platforms.
    if (!memcmp(Str, "word", 4))
      DestWidth = S.Context.Target.getPointerWidth(0);
    if (!memcmp(Str, "byte", 4))
      DestWidth = S.Context.Target.getCharWidth();
    break;
  case 7:
    if (!memcmp(Str, "pointer", 7))
      DestWidth = S.Context.Target.getPointerWidth(0);
    break;
  }

  QualType OldTy;
  if (TypedefDecl *TD = dyn_cast<TypedefDecl>(D))
    OldTy = TD->getUnderlyingType();
  else if (ValueDecl *VD = dyn_cast<ValueDecl>(D))
    OldTy = VD->getType();
  else {
    S.Diag(D->getLocation(), diag::err_attr_wrong_decl)
      << "mode" << SourceRange(Attr.getLoc(), Attr.getLoc());
    return;
  }

  if (!OldTy->getAsBuiltinType() && !OldTy->isComplexType())
    S.Diag(Attr.getLoc(), diag::err_mode_not_primitive);
  else if (IntegerMode) {
    if (!OldTy->isIntegralType())
      S.Diag(Attr.getLoc(), diag::err_mode_wrong_type);
  } else if (ComplexMode) {
    if (!OldTy->isComplexType())
      S.Diag(Attr.getLoc(), diag::err_mode_wrong_type);
  } else {
    if (!OldTy->isFloatingType())
      S.Diag(Attr.getLoc(), diag::err_mode_wrong_type);
  }

  // FIXME: Sync this with InitializePredefinedMacros; we need to match
  // int8_t and friends, at least with glibc.
  // FIXME: Make sure 32/64-bit integers don't get defined to types of
  // the wrong width on unusual platforms.
  // FIXME: Make sure floating-point mappings are accurate
  // FIXME: Support XF and TF types
  QualType NewTy;
  switch (DestWidth) {
  case 0:
    S.Diag(Attr.getLoc(), diag::err_unknown_machine_mode) << Name;
    return;
  default:
    S.Diag(Attr.getLoc(), diag::err_unsupported_machine_mode) << Name;
    return;
  case 8:
    if (!IntegerMode) {
      S.Diag(Attr.getLoc(), diag::err_unsupported_machine_mode) << Name;
      return;
    }
    if (OldTy->isSignedIntegerType())
      NewTy = S.Context.SignedCharTy;
    else
      NewTy = S.Context.UnsignedCharTy;
    break;
  case 16:
    if (!IntegerMode) {
      S.Diag(Attr.getLoc(), diag::err_unsupported_machine_mode) << Name;
      return;
    }
    if (OldTy->isSignedIntegerType())
      NewTy = S.Context.ShortTy;
    else
      NewTy = S.Context.UnsignedShortTy;
    break;
  case 32:
    if (!IntegerMode)
      NewTy = S.Context.FloatTy;
    else if (OldTy->isSignedIntegerType())
      NewTy = S.Context.IntTy;
    else
      NewTy = S.Context.UnsignedIntTy;
    break;
  case 64:
    if (!IntegerMode)
      NewTy = S.Context.DoubleTy;
    else if (OldTy->isSignedIntegerType())
      NewTy = S.Context.LongLongTy;
    else
      NewTy = S.Context.UnsignedLongLongTy;
    break;
  case 96:
    NewTy = S.Context.LongDoubleTy;
    break;
  case 128:
    if (!IntegerMode) {
      S.Diag(Attr.getLoc(), diag::err_unsupported_machine_mode) << Name;
      return;
    }
    NewTy = S.Context.getFixedWidthIntType(128, OldTy->isSignedIntegerType());
    break;
  }

  if (ComplexMode) {
    NewTy = S.Context.getComplexType(NewTy);
  }

  // Install the new type.
  if (TypedefDecl *TD = dyn_cast<TypedefDecl>(D))
    TD->setUnderlyingType(NewTy);
  else
    cast<ValueDecl>(D)->setType(NewTy);
}

static void HandleNodebugAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() > 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }

  if (!isFunctionOrMethod(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "nodebug" << 0 /*function*/;
    return;
  }
  
  d->addAttr(::new (S.Context) NodebugAttr());
}

static void HandleNoinlineAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  if (!isa<FunctionDecl>(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
    << "noinline" << 0 /*function*/;
    return;
  }
  
  d->addAttr(::new (S.Context) NoinlineAttr());
}

static void HandleGNUCInlineAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 0;
    return;
  }
  
  FunctionDecl *Fn = dyn_cast<FunctionDecl>(d);
  if (Fn == 0) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
      << "gnuc_inline" << 0 /*function*/;
    return;
  }
  
  if (!Fn->isInline()) {
    S.Diag(Attr.getLoc(), diag::warn_gnuc_inline_attribute_requires_inline);
    return;
  }
  
  if (Fn->getStorageClass() == FunctionDecl::Extern) {
    S.Diag(Attr.getLoc(), diag::warn_gnuc_inline_attribute_extern_inline);
    return;
  }
  
  d->addAttr(::new (S.Context) GNUCInlineAttr());
}

static void HandleRegparmAttr(Decl *d, const AttributeList &Attr, Sema &S) {
  // check the attribute arguments.
  if (Attr.getNumArgs() != 1) {
    S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments) << 1;
    return;
  }

  if (!isFunctionOrMethod(d)) {
    S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type)
    << "regparm" << 0 /*function*/;
    return;
  }

  Expr *NumParamsExpr = static_cast<Expr *>(Attr.getArg(0));
  llvm::APSInt NumParams(32);
  if (!NumParamsExpr->isIntegerConstantExpr(NumParams, S.Context)) {
    S.Diag(Attr.getLoc(), diag::err_attribute_argument_not_int)
      << "regparm" << NumParamsExpr->getSourceRange();
    return;
  }

  if (S.Context.Target.getRegParmMax() == 0) {
    S.Diag(Attr.getLoc(), diag::err_attribute_regparm_wrong_platform)
      << NumParamsExpr->getSourceRange();
    return;
  }

  if (NumParams.getLimitedValue(255) > S.Context.Target.getRegParmMax()) {
    S.Diag(Attr.getLoc(), diag::err_attribute_regparm_invalid_number)
      << S.Context.Target.getRegParmMax() << NumParamsExpr->getSourceRange();
    return;
  }

  d->addAttr(::new (S.Context) RegparmAttr(NumParams.getZExtValue()));
}

//===----------------------------------------------------------------------===//
// Top Level Sema Entry Points
//===----------------------------------------------------------------------===//

/// ProcessDeclAttribute - Apply the specific attribute to the specified decl if
/// the attribute applies to decls.  If the attribute is a type attribute, just
/// silently ignore it.
static void ProcessDeclAttribute(Decl *D, const AttributeList &Attr, Sema &S) {
  switch (Attr.getKind()) {
  case AttributeList::AT_IBOutlet:    HandleIBOutletAttr  (D, Attr, S); break;
  case AttributeList::AT_address_space:
  case AttributeList::AT_objc_gc:
    // Ignore these, these are type attributes, handled by ProcessTypeAttributes.
    break;
  case AttributeList::AT_alias:       HandleAliasAttr     (D, Attr, S); break;
  case AttributeList::AT_aligned:     HandleAlignedAttr   (D, Attr, S); break;
  case AttributeList::AT_always_inline: 
    HandleAlwaysInlineAttr  (D, Attr, S); break;
  case AttributeList::AT_analyzer_noreturn:
    HandleAnalyzerNoReturnAttr  (D, Attr, S); break;  
  case AttributeList::AT_annotate:    HandleAnnotateAttr  (D, Attr, S); break;
  case AttributeList::AT_constructor: HandleConstructorAttr(D, Attr, S); break;
  case AttributeList::AT_deprecated:  HandleDeprecatedAttr(D, Attr, S); break;
  case AttributeList::AT_destructor:  HandleDestructorAttr(D, Attr, S); break;
  case AttributeList::AT_dllexport:   HandleDLLExportAttr (D, Attr, S); break;
  case AttributeList::AT_dllimport:   HandleDLLImportAttr (D, Attr, S); break;
  case AttributeList::AT_ext_vector_type:
    HandleExtVectorTypeAttr(D, Attr, S);
    break;
  case AttributeList::AT_fastcall:    HandleFastCallAttr  (D, Attr, S); break;
  case AttributeList::AT_format:      HandleFormatAttr    (D, Attr, S); break;
  case AttributeList::AT_gnuc_inline: HandleGNUCInlineAttr(D, Attr, S); break;
  case AttributeList::AT_mode:        HandleModeAttr      (D, Attr, S); break;
  case AttributeList::AT_nonnull:     HandleNonNullAttr   (D, Attr, S); break;
  case AttributeList::AT_noreturn:    HandleNoReturnAttr  (D, Attr, S); break;
  case AttributeList::AT_nothrow:     HandleNothrowAttr   (D, Attr, S); break;
  case AttributeList::AT_packed:      HandlePackedAttr    (D, Attr, S); break;
  case AttributeList::AT_section:     HandleSectionAttr   (D, Attr, S); break;
  case AttributeList::AT_stdcall:     HandleStdCallAttr   (D, Attr, S); break;
  case AttributeList::AT_unavailable: HandleUnavailableAttr(D, Attr, S); break;
  case AttributeList::AT_unused:      HandleUnusedAttr    (D, Attr, S); break;
  case AttributeList::AT_used:        HandleUsedAttr      (D, Attr, S); break;
  case AttributeList::AT_vector_size: HandleVectorSizeAttr(D, Attr, S); break;
  case AttributeList::AT_visibility:  HandleVisibilityAttr(D, Attr, S); break;
  case AttributeList::AT_warn_unused_result: HandleWarnUnusedResult(D,Attr,S);
    break;
  case AttributeList::AT_weak:        HandleWeakAttr      (D, Attr, S); break;
  case AttributeList::AT_weak_import: HandleWeakImportAttr(D, Attr, S); break;
  case AttributeList::AT_transparent_union:
    HandleTransparentUnionAttr(D, Attr, S);
    break;
  case AttributeList::AT_objc_exception:
    HandleObjCExceptionAttr(D, Attr, S);
    break;
  case AttributeList::AT_overloadable:HandleOverloadableAttr(D, Attr, S); break;
  case AttributeList::AT_nsobject:    HandleObjCNSObject  (D, Attr, S); break;
  case AttributeList::AT_blocks:      HandleBlocksAttr    (D, Attr, S); break;
  case AttributeList::AT_sentinel:    HandleSentinelAttr  (D, Attr, S); break;
  case AttributeList::AT_const:       HandleConstAttr     (D, Attr, S); break;
  case AttributeList::AT_pure:        HandlePureAttr      (D, Attr, S); break;
  case AttributeList::AT_cleanup:     HandleCleanupAttr   (D, Attr, S); break;
  case AttributeList::AT_nodebug:     HandleNodebugAttr   (D, Attr, S); break;
  case AttributeList::AT_noinline:    HandleNoinlineAttr  (D, Attr, S); break;
  case AttributeList::AT_regparm:     HandleRegparmAttr   (D, Attr, S); break;
  case AttributeList::IgnoredAttribute: 
    // Just ignore
    break;
  default:
    S.Diag(Attr.getLoc(), diag::warn_attribute_ignored) << Attr.getName();
    break;
  }
}

/// ProcessDeclAttributeList - Apply all the decl attributes in the specified
/// attribute list to the specified decl, ignoring any type attributes.
void Sema::ProcessDeclAttributeList(Decl *D, const AttributeList *AttrList) {
  while (AttrList) {
    ProcessDeclAttribute(D, *AttrList, *this);
    AttrList = AttrList->getNext();
  }
}


/// ProcessDeclAttributes - Given a declarator (PD) with attributes indicated in
/// it, apply them to D.  This is a bit tricky because PD can have attributes
/// specified in many different places, and we need to find and apply them all.
void Sema::ProcessDeclAttributes(Decl *D, const Declarator &PD) {
  // Apply decl attributes from the DeclSpec if present.
  if (const AttributeList *Attrs = PD.getDeclSpec().getAttributes())
    ProcessDeclAttributeList(D, Attrs);
  
  // Walk the declarator structure, applying decl attributes that were in a type
  // position to the decl itself.  This handles cases like:
  //   int *__attr__(x)** D;
  // when X is a decl attribute.
  for (unsigned i = 0, e = PD.getNumTypeObjects(); i != e; ++i)
    if (const AttributeList *Attrs = PD.getTypeObject(i).getAttrs())
      ProcessDeclAttributeList(D, Attrs);
  
  // Finally, apply any attributes on the decl itself.
  if (const AttributeList *Attrs = PD.getAttributes())
    ProcessDeclAttributeList(D, Attrs);
}

