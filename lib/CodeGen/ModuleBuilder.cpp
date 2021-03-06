//===--- ModuleBuilder.cpp - Emit LLVM Code from ASTs ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This builds an AST and converts it to LLVM Code.
//
//===----------------------------------------------------------------------===//

#include "clang/CodeGen/ModuleBuilder.h"
#include "CodeGenModule.h"
#include "clang/Frontend/CompileOptions.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/Module.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Support/Compiler.h"
#include "llvm/ADT/OwningPtr.h"
using namespace clang;


namespace {
  class VISIBILITY_HIDDEN CodeGeneratorImpl : public CodeGenerator {
    Diagnostic &Diags;
    llvm::OwningPtr<const llvm::TargetData> TD;
    ASTContext *Ctx;
    const CompileOptions CompileOpts;  // Intentionally copied in.
  protected:
    llvm::OwningPtr<llvm::Module> M;
    llvm::OwningPtr<CodeGen::CodeGenModule> Builder;
  public:
    CodeGeneratorImpl(Diagnostic &diags, const std::string& ModuleName,
                      const CompileOptions &CO)
      : Diags(diags), CompileOpts(CO), M(new llvm::Module(ModuleName)) {}
    
    virtual ~CodeGeneratorImpl() {}
    
    virtual llvm::Module* GetModule() {
      return M.get();
    }
    
    virtual llvm::Module* ReleaseModule() {
      return M.take();
    }
    
    virtual void Initialize(ASTContext &Context) {
      Ctx = &Context;
      
      M->setTargetTriple(Ctx->Target.getTargetTriple());
      M->setDataLayout(Ctx->Target.getTargetDescription());
      TD.reset(new llvm::TargetData(Ctx->Target.getTargetDescription()));
      Builder.reset(new CodeGen::CodeGenModule(Context, CompileOpts,
                                               *M, *TD, Diags));
    }
    
    virtual void HandleTopLevelDecl(DeclGroupRef DG) {
      // Make sure to emit all elements of a Decl.
      for (DeclGroupRef::iterator I = DG.begin(), E = DG.end(); I != E; ++I)
        Builder->EmitTopLevelDecl(*I);
    }

    /// HandleTagDeclDefinition - This callback is invoked each time a TagDecl
    /// to (e.g. struct, union, enum, class) is completed. This allows the
    /// client hack on the type, which can occur at any point in the file
    /// (because these can be defined in declspecs).
    virtual void HandleTagDeclDefinition(TagDecl *D) {
      Builder->UpdateCompletedType(D);
    }

    virtual void HandleTranslationUnit(ASTContext &Ctx) {
      if (Diags.hasErrorOccurred()) {
        M.reset();
        return;
      }

      if (Builder)
        Builder->Release();
    };
  };
}

CodeGenerator *clang::CreateLLVMCodeGen(Diagnostic &Diags, 
                                        const std::string& ModuleName,
                                        const CompileOptions &CO) {
  return new CodeGeneratorImpl(Diags, ModuleName, CO);
}
