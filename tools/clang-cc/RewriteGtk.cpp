/* -*  indent-tabs-mode:nil;  -*-
 * RewriteGtk.cpp - refactoring tool to move code from GTK+ 2.x to 3.x
 * Based on RewriteObjC.cpp, distributed under the University of
 * Illinois Open Source License. See LICENSE.TXT in clang for details.
 */

#include "ASTConsumers.h"
#include "clang/Rewrite/Rewriter.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ParentMap.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/Diagnostic.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Streams.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/System/Path.h"
#include <fstream>
#include <sstream>
#include <stdio.h>

using namespace clang;

namespace {
  struct LocalReferenceItem {
    std::string refType;
    std::string localName;
    std::string accessor;
    std::string objName;

    LocalReferenceItem(const std::string& refType, const std::string& localName,
                       const std::string& accessor, const std::string& objName)
      : refType(refType), localName(localName), accessor(accessor), objName(objName)
    {
    }

    LocalReferenceItem(const LocalReferenceItem& item)
      : refType(item.refType), localName(item.localName), accessor(item.accessor), objName(item.objName)
    {
    }

    bool operator==(const LocalReferenceItem& R) const { return localName == R.localName; }
    bool operator>(const LocalReferenceItem& R) const { return localName > R.localName; }
    bool operator<(const LocalReferenceItem& R) const { return localName < R.localName; }
  };

  class RewriteItem {
    std::string klass;
    std::string member;

  public:
    std::string comment;
    std::string accessor;
    std::string getRefType;
    std::string setter;
    bool appendNullArg;

    RewriteItem(const std::string& klass, const std::string& member,
                const std::string& comment, const std::string& accessor,
                const std::string& getRefType, const std::string& setter,
                bool appendNullArg)
      : klass(klass), member(member), comment(comment), accessor(accessor),
	getRefType(getRefType), setter(setter), appendNullArg(appendNullArg)
    {
    }

    static std::string getKey(std::string type, std::string member) {
      return type + "::" + member;
    }

    std::string getKey() {
      return getKey(klass + " *", member);
    }

    std::string getFormattedAccessor(std::string var) {
      std::string str = accessor + " (" + var;
      if (appendNullArg)
        str.append(", NULL");
      str.append(")");
      return str;
    }

    std::string getFormattedComment() {
      if (comment.empty())
        return "";
      else
        return "/* REWRITE: " + comment + " */";
    }

    bool hasGetRef() {
      return !getRefType.empty();
    }
  };

  class RewriteGtk : public ASTConsumer {
    Rewriter Rewrite;
    Diagnostic &Diags;
    unsigned RewriteFailedDiag;

    ASTContext *Context;
    SourceManager *SM;
    TranslationUnitDecl *TUDecl;
    FileID MainFileID;
    const char *MainFileStart, *MainFileEnd;

    Stmt *lastStmt; /* To keep track of where to insert rewrite comments. */

    llvm::OwningPtr<ParentMap> PM;

    bool InsertedGtkHeader;

    std::string InFileName;
    std::string OutFileName;

    std::map<std::string, RewriteItem *> rewriteItemMap;

    std::vector<LocalReferenceItem> newLocals;
    std::vector<std::string> existingLocals;

  public:
    virtual void Initialize(ASTContext &context);

    // Top Level Driver code.
    virtual void HandleTopLevelDecl(DeclGroupRef D) {
      for (DeclGroupRef::iterator I = D.begin(), E = D.end(); I != E; ++I)
        HandleTopLevelSingleDecl(*I);
    }

    void HandleTopLevelSingleDecl(Decl *D);

    void HandleDeclInMainFile(Decl *D);
    RewriteGtk(std::string inFile, std::string outFile, Diagnostic &D, const LangOptions &LOpts);
    ~RewriteGtk() { };

    virtual void HandleTranslationUnit(ASTContext& C);

    void InsertText(SourceLocation Loc, const char *StrData, unsigned StrLen, bool InsertAfter = true)
    {
      // If insertion succeeded or warning disabled return with no warning.
      if (!Rewrite.InsertText(Loc, StrData, StrLen, InsertAfter))
        return;

      Diags.Report(Context->getFullLoc(Loc), RewriteFailedDiag);
    }

    void RemoveText(SourceLocation Loc, unsigned StrLen)
    {
      // If removal succeeded or warning disabled return with no warning.
      if (!Rewrite.RemoveText(Loc, StrLen))
        return;

      Diags.Report(Context->getFullLoc(Loc), RewriteFailedDiag);
    }

    void ReplaceText(SourceLocation Start, unsigned OrigLength, const char *NewStr, unsigned NewLength)
    {
      // If removal succeeded or warning disabled return with no warning.
      if (!Rewrite.ReplaceText(Start, OrigLength, NewStr, NewLength))
        return;

      Diags.Report(Context->getFullLoc(Start), RewriteFailedDiag);
    }

    std::string GetRewriteItemAttribute(std::string line, const char *str)
    {
      size_t start, end;
      std::string match = str;

      match.append("[");

      start = line.find(match);
      if (start != std::string::npos) {
        start += match.size();
        end = line.find("]", start);
        if (end != std::string::npos) {
          return line.substr(start, end - start);
        }
      }

      return "";
    }

    std::string GetRewriteItemAttributeString(std::string line, const char *str)
    {
      std::string tmp = GetRewriteItemAttribute(line, str);
      if (tmp.empty())
        tmp = strdup ("");
      return tmp;
    }

    bool GetRewriteItemAttributeBool(std::string line, const char *str)
    {
      bool ret = false;
      std::string tmp = GetRewriteItemAttribute(line, str);

      if (!tmp.empty()) {
        ret = true;
      }

      return ret;
    }

    void InsertComment(std::string comment);
    bool HandleUnaryOperator(Stmt *stmt, QualType returnType, std::string accessor);
    bool HandleBinaryOperator(Stmt *stmt, std::string accessor);
    bool HandleCompoundAssignOperator(Stmt *stmt, std::string accessor);

    void RewriteInclude();
    Stmt *RewriteFunctionBodyOrGlobalInitializer(Stmt *S, int depth);
    std::string CreateUniqueLocalName(std::string proposedName,
				      std::vector<std::string>& reservedNames);
  };
}

RewriteGtk::RewriteGtk(std::string inFile, std::string outFile, Diagnostic &D, const LangOptions &LOpts)
  : Diags(D)
{
  InFileName = inFile;
  OutFileName = outFile;
  RewriteFailedDiag = Diags.getCustomDiagID(Diagnostic::Warning,
                                            "rewriting sub-expression within a macro (may not be correct)");

  std::ifstream infile ("/usr/local/share/clang-gtk/gtk.rewrites", std::ios_base::in);
  std::string line;
  std::string klass;

  while (getline(infile, line)) {
    std::string member, accessor, setter, comment, function, getRefType;
    bool appendNullArg;

    // Skip comments and empty lines.
    if (line[0] == '#' || line.empty()) {
      continue;
    }

    // Get a new class.
    std::string tmp = line.substr (0, 7);
    if (tmp == "@class ") {
      klass = line.substr(7);

      if (klass.empty())
        klass.clear();
      continue;
    }

    if (klass.empty())
      continue;

    member = GetRewriteItemAttribute(line, "get");
    accessor = GetRewriteItemAttribute(line, "accessor");
    setter = GetRewriteItemAttribute(line, "setter");
    comment = GetRewriteItemAttributeString(line, "comment");
    appendNullArg = GetRewriteItemAttributeBool(line, "append-null-arg");
    function = GetRewriteItemAttribute(line, "function");
    getRefType = GetRewriteItemAttribute(line, "getref");

    if (!member.empty() && !accessor.empty()) {
      RewriteItem *item = new RewriteItem (klass.c_str(),
                                           member, comment,
                                           accessor, getRefType,
                                           setter,
                                           appendNullArg);
      rewriteItemMap[item->getKey()] = item;
    }
    else if (!function.empty() && !accessor.empty()) {
      // ...
    }
  }
}

ASTConsumer *clang::CreateCodeRewriterGtk(const std::string &InFile,
                                          const std::string &OutFile,
                                          Diagnostic &Diags,
                                          const LangOptions &LOpts)
{
  return new RewriteGtk(InFile, OutFile, Diags, LOpts);
}

void RewriteGtk::Initialize(ASTContext &context)
{
  Context = &context;
  SM = &Context->getSourceManager();
  TUDecl = Context->getTranslationUnitDecl();

  // Get the ID and start/end of the main file.
  MainFileID = SM->getMainFileID();
  const llvm::MemoryBuffer *MainBuf = SM->getBuffer(MainFileID);
  MainFileStart = MainBuf->getBufferStart();
  MainFileEnd = MainBuf->getBufferEnd();

  Rewrite.setSourceMgr(Context->getSourceManager(), Context->getLangOptions());
}

void RewriteGtk::HandleTopLevelSingleDecl(Decl *D)
{
  // Two cases: either the decl could be in the main file, or it could be in a
  // #included file.  If the former, rewrite it now.  If the later, check to see
  // if we rewrote the #include/#import.
  SourceLocation Loc = D->getLocation();
  Loc = SM->getInstantiationLoc(Loc);

  // If this is for a builtin, ignore it.
  if (Loc.isInvalid()) return;

  // If we have a decl in the main file, see if we should rewrite it.
  if (SM->isFromMainFile(Loc)) {
    // Keep a parent map for every function so we can backtrack
    // statements to see what context they are in.
    if (D->getKind() == Decl::Function) {
      FunctionDecl *functionDecl = cast<FunctionDecl>(D);
      Stmt *body = functionDecl->getBody();

      PM.reset(new ParentMap(body));
    }

    return HandleDeclInMainFile(D);
  }
}

/// HandleDeclInMainFile - This is called for each top-level decl defined in the
/// main file of the input.
void RewriteGtk::HandleDeclInMainFile(Decl *D)
{
  std::vector<LocalReferenceItem>::iterator iter, end;

  if (FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    if (CompoundStmt *body = FD->getBody()) {
      body = cast_or_null<CompoundStmt>(RewriteFunctionBodyOrGlobalInitializer(body, 0));
      FD->setBody(body);
    }
  }

  if (!newLocals.empty()) {
    for (iter = newLocals.begin(), end = newLocals.end(); iter != end; iter++)
      {
        printf (" ======= Iter: %s, %s ============\n", (*iter).refType.c_str(), (*iter).localName.c_str());
      }
  }
}

void RewriteGtk::HandleTranslationUnit(ASTContext& C)
{
  if (Diags.hasErrorOccurred())
    return;

  // Create the output file.
  llvm::OwningPtr<llvm::raw_ostream> OwnedStream;
  llvm::raw_ostream *OutFile;

  if (OutFileName == "-") {
    OutFile = &llvm::outs();
  } else if (!OutFileName.empty()) {
    std::string Err;
    OutFile = new llvm::raw_fd_ostream(OutFileName.c_str(),
				       true,
				       Err);
    OwnedStream.reset(OutFile);
  } else {
    llvm::sys::Path Path(InFileName);
    Path.eraseSuffix();
    Path.appendSuffix("c");
    std::string Err;
    OutFile = new llvm::raw_fd_ostream(Path.toString().c_str(),
				       true,
				       Err);
    OwnedStream.reset(OutFile);
  }

  RewriteInclude();

  // Get the buffer corresponding to MainFileID.  If we haven't changed it, then
  // we are done.
  if (const RewriteBuffer *RewriteBuf = Rewrite.getRewriteBufferFor(MainFileID)) {
    *OutFile << std::string(RewriteBuf->begin(), RewriteBuf->end());
    OutFile->flush();
  } else {
    fprintf(stderr, "No changes\n");
    if (!OutFileName.empty())
      unlink(OutFileName.c_str());
  }
}

void RewriteGtk::RewriteInclude()
{
  SourceLocation locStart = SM->getLocForStartOfFile(MainFileID);
  std::pair<const char*, const char*> mainBuf = SM->getBufferData(MainFileID);
  const char *mainBufStart = mainBuf.first;
  const char *mainBufEnd = mainBuf.second;
  size_t includeLen = strlen("include <gtk/gtk");

  // Loop over the whole file, looking for includes.
  for (const char *bufPtr = mainBufStart; bufPtr < mainBufEnd; ++bufPtr) {
    if (*bufPtr == '#') {
      if (++bufPtr == mainBufEnd)
        return;

      if (strncmp(bufPtr, "include <gtk/gtk", includeLen) == 0) {
        const char *endBracketPtr = strchr(bufPtr + includeLen, '>');
        if (endBracketPtr && endBracketPtr-bufPtr < 128) {
          SourceLocation loc = locStart.getFileLocWithOffset(bufPtr - mainBufStart - 1);

          RemoveText(loc, std::min(endBracketPtr + 3, mainBufEnd) - bufPtr);

          if (!InsertedGtkHeader) {
            InsertText(loc, "#include <gtk/gtk.h>\n", 21, false);
            InsertedGtkHeader = true;
          }
        }
      }
    }
  }
}

void RewriteGtk::InsertComment(std::string comment)
{
  SourceLocation commentLoc = lastStmt->getLocStart();
  const char *endBuf, *startBuf, *prevBuf;

  if (getenv("REWRITE_GTK_NO_COMMENTS"))
    return;

  // Get the leading whitespace characters on the line and copy them
  // for the newly inserted one.
  startBuf = endBuf = SM->getCharacterData(commentLoc);
  while (*startBuf != '\n')
    startBuf--;

  // Prevent the same comment from being inserted again by checking if
  // the line above has the same comment already.
  prevBuf = startBuf - 1;
  while (*prevBuf != '\n')
    prevBuf--;
  prevBuf++;

  std::string prevLine = std::string(prevBuf, startBuf);

  // Strip whitespace.
  size_t first = prevLine.find_first_not_of(" \t");
  if (first != std::string::npos)
    prevLine = prevLine.substr(first, prevLine.find_last_not_of(" \t") - first + 1);

  if (prevLine != comment)
    {
      // Replacing all newlines in the comment with the indentation prefix.
      size_t i = 0;
      while ((i = comment.find('\n', i)) != std::string::npos)
        {
          comment = comment.insert(i+1, std::string(startBuf+1, endBuf));
          i += endBuf - startBuf;
        }

      comment.append(startBuf, endBuf);

      InsertText(commentLoc, comment.c_str(), comment.size());
    }
}

bool RewriteGtk::HandleUnaryOperator(Stmt *stmt, QualType returnType, std::string accessor)
{
  Stmt *parentStmt = PM->getParent(stmt);
  while (parentStmt != NULL) {
    if (UnaryOperator *op = dyn_cast<UnaryOperator>(parentStmt)) {
      Type *typePtr = returnType.getTypePtr();

      switch(op->getOpcode()) {
      case UnaryOperator::AddrOf:
        // Allow taking the address of the result if it is a pointer type.
        if (typePtr->isPointerType())
          return false;

        InsertComment(std::string("/* REWRITE: Use an accessor function instead of direct access.\n"
                                  "   Also change the code no to take the address of the return value\n"
                                  "   since that does not work accessors that return non-pointer types.\n"
                                  "   " + accessor + "\n"
                                  " */"));
        return true;

      case UnaryOperator::Deref:
	//case UnaryOperator::SizeOf:
	//case UnaryOperator::AlignOf:
      case UnaryOperator::Real:
      case UnaryOperator::Imag:
      case UnaryOperator::Extension:
      case UnaryOperator::OffsetOf:
      case UnaryOperator::Plus:
      case UnaryOperator::Minus:
      case UnaryOperator::Not:
      case UnaryOperator::LNot:
        return false;

      case UnaryOperator::PostInc:
      case UnaryOperator::PostDec:
      case UnaryOperator::PreInc:
      case UnaryOperator::PreDec:
        InsertComment(std::string("/* REWRITE: Use an accessor function instead of direct access.\n"
                                  "   Automatic rewriting is not possible due to the use of an\n"
                                  "   increment/decrement operator on the return value.\n"
                                  "   " + accessor + "\n"
                                  " */"));
        return true;

      default:
        return false;
      }
    }

    if (isa<ParenExpr>(parentStmt) || isa<MemberExpr>(parentStmt)) {
      stmt = parentStmt;
      parentStmt = PM->getParent(parentStmt);
    } else
      break;
  }

  return false;
}

bool RewriteGtk::HandleBinaryOperator(Stmt *stmt, std::string accessor)
{
  Stmt *parentStmt = PM->getParent(stmt);
  if (BinaryOperator *op = dyn_cast<BinaryOperator>(parentStmt)) {
    if (op->getLHS() == stmt) {
      switch(op->getOpcode()) {
      case BinaryOperator::Assign:
      case BinaryOperator::MulAssign:
      case BinaryOperator::DivAssign:
      case BinaryOperator::RemAssign:
      case BinaryOperator::AddAssign:
      case BinaryOperator::SubAssign:
      case BinaryOperator::ShlAssign:
      case BinaryOperator::ShrAssign:
      case BinaryOperator::AndAssign:
      case BinaryOperator::XorAssign:
      case BinaryOperator::OrAssign:
        InsertComment(std::string("/* REWRITE: Use an accessor function instead of direct access.\n"
                                  "   Also change the code not to assign to the return value.\n"
                                  "   " + accessor + "\n"
                                  " */"));
        return true;
      default:
        break;
      }
    }
  }

  return false;
}

bool RewriteGtk::HandleCompoundAssignOperator(Stmt *stmt, std::string accessor)
{
  Stmt *parentStmt = PM->getParent(stmt);
  while (parentStmt != NULL) {
    if (CompoundAssignOperator *op = dyn_cast<CompoundAssignOperator>(parentStmt)) {
      if (op->getLHS() == stmt) {
        switch(op->getOpcode()) {
        case BinaryOperator::Assign:
        case BinaryOperator::MulAssign:
        case BinaryOperator::DivAssign:
        case BinaryOperator::RemAssign:
        case BinaryOperator::AddAssign:
        case BinaryOperator::SubAssign:
        case BinaryOperator::ShlAssign:
        case BinaryOperator::ShrAssign:
        case BinaryOperator::AndAssign:
        case BinaryOperator::XorAssign:
        case BinaryOperator::OrAssign:
          InsertComment(std::string("/* REWRITE: Use an accessor function instead of direct access.\n"
                                    "   Also change the code not to assign to the return value.\n"
                                    "   " + accessor + "\n"
                                    " */"));
          return true;

        default:
          return false;
        }
      }
    }

    if (isa<ParenExpr>(parentStmt) || isa<MemberExpr>(parentStmt)) {
      stmt = parentStmt;
      parentStmt = PM->getParent(parentStmt);
    } else
      break;
  }

  return false;
}

// When we're injecting new local variables at the top of the function,
// make sure the local variable is unique.  Just append a number to the
// end of it.
std::string RewriteGtk::CreateUniqueLocalName(std::string proposedName,
					      std::vector<std::string>& reservedNames)
{
  std::vector<std::string>::iterator iter;
  std::string name = proposedName;
  int n = 0;

  do
    {
      iter = find(reservedNames.begin(), reservedNames.end(), name);

      if (iter == reservedNames.end())
        {
          return name;
        }
      else
        {
          std::stringstream ss;
          ss << proposedName << n;
          name = ss.str();
          n++;
        }
    } while (1);
}

/* Handle rewriting direct accesses of member fields to use accessors
 * instead. We look for occurances of MemberExpr whose FieldDecl's
 * name and type matches one of our rewrite candidates.
 */
Stmt *RewriteGtk::RewriteFunctionBodyOrGlobalInitializer(Stmt *stmt, int depth)
{
  Stmt *lastLocalDecl = NULL;

  // Start by rewriting all children.
  for (Stmt::child_iterator childIter = stmt->child_begin(), E = stmt->child_end();
       childIter != E; ++childIter)
    {
      if (*childIter)
        {
          if (isa<CompoundStmt>(stmt))
            {
              lastStmt = *childIter;
            }

          if (isa<DeclStmt>(*childIter))
            {
              DeclStmt *decl_stmt = dyn_cast<DeclStmt>(*childIter);
              lastLocalDecl = *childIter;

              if (decl_stmt->isSingleDecl() && depth == 0)
                {
                  Decl *decl = decl_stmt->getSingleDecl();

                  if (isa<NamedDecl>(decl))
                    {
                      // Build a list of local variables to check newLocals against for
                      // variable naming conflicts before injecting new variables.
                      NamedDecl* named = dyn_cast<NamedDecl>(decl);

                      existingLocals.push_back (named->getNameAsString());
                    }
                }
            }

          Stmt *newStmt = RewriteFunctionBodyOrGlobalInitializer(*childIter, depth + 1);
          if (newStmt)
            *childIter = newStmt;
        }
    }

  if (MemberExpr *memberExpr = dyn_cast<MemberExpr>(stmt))
    {
      if (memberExpr->isArrow())
        {
          Expr *base = memberExpr->getBase()->IgnoreParens();
          FieldDecl *memberDecl = dyn_cast<FieldDecl>(memberExpr->getMemberDecl());
          const char *memberName = memberDecl->getNameAsCString();

          if (DeclRefExpr *declRefExpr = dyn_cast<DeclRefExpr>(base))
            {
              ValueDecl *valueDecl = dyn_cast<ValueDecl>(declRefExpr->getDecl());
              QualType type = valueDecl->getType();
              const std::string declName = valueDecl->getNameAsString();

              RewriteItem *item = rewriteItemMap[RewriteItem::getKey(type.getAsString(), memberName)];

              if (item == NULL)
                return stmt;

              if (HandleUnaryOperator(stmt, memberDecl->getType(), item->getFormattedAccessor(declName)) ||
                  HandleBinaryOperator(stmt, item->getFormattedAccessor(declName)) ||
                  HandleCompoundAssignOperator(stmt, item->getFormattedAccessor(declName)))
                return stmt;

              // FIXME: I think that the phys part here can be moved to just
              // the replacetext call since getchardata does it already...
              //
              if (item->hasGetRef ())
                {
                  SourceLocation start = stmt->getLocStart();
                  SourceLocation end = stmt->getLocEnd();
                  SourceLocation instStart = SM->getInstantiationLoc(start);
                  SourceLocation instEnd = SM->getInstantiationLoc(end);
                  std::string ref = item->getRefType;
                  std::string localName = declName + "_" + memberName;
                  const char* startBuf;
                  const char* endBuf;

                  LocalReferenceItem local_item(ref, localName, item->accessor, declName);

                  newLocals.push_back(local_item);

                  if (start != instStart ||
                      end != instEnd)
                    {
                      InsertComment(std::string("/* REWRITE: Can't rewrite some or all of the next expression due\n"
                                                " *          to macro expansions. Use " + localName + ".\n"
                                                " */"));
                    }

                  startBuf = SM->getCharacterData(start);
                  endBuf = SM->getCharacterData(end) + strlen(memberName);

                  ReplaceText(start, endBuf - startBuf, localName.c_str(), localName.size());
                }
              else
                {
                  SourceLocation start = stmt->getLocStart();
                  SourceLocation end = stmt->getLocEnd();
                  SourceLocation instStart = SM->getInstantiationLoc(start);
                  SourceLocation instEnd = SM->getInstantiationLoc(end);

                  if (start != instStart ||
                      end != instEnd)
                    {
                      InsertComment(std::string("/* REWRITE: Can't rewrite some or all of the next expression due\n"
                                                " *          to macro expansions. Use " + item->getFormattedAccessor(declName) + ".\n"
                                                " */"));
                    }

                  const char *startBuf = SM->getCharacterData(start);
                  const char *endBuf = SM->getCharacterData(end) + strlen(memberName);

                  std::string str = item->getFormattedAccessor(declName);

                  if (!item->accessor.empty())
                    ReplaceText(start, endBuf - startBuf, str.c_str(), str.size());

                  if (lastStmt && !item->comment.empty())
                    {
                      InsertComment(item->getFormattedComment());
                    }
                }

              return stmt;
            }
          else if (CastExpr *castExpr = dyn_cast<CastExpr>(base))
            {
              QualType type = castExpr->getType();

              RewriteItem *item = rewriteItemMap[RewriteItem::getKey(type.getAsString(), memberName)];
              if (item == NULL)
                return stmt;

              SourceLocation startLoc = stmt->getLocStart();
              SourceLocation endLoc = stmt->getLocEnd();

              // Need to get logical loc since the loc is pointing to the
              // macro definition.
              startLoc = SM->getInstantiationLoc(startLoc);
              endLoc = SM->getInstantiationLoc(endLoc);

              const char *startBuf = SM->getCharacterData(startLoc);
              const char *endBuf = SM->getCharacterData(endLoc);

              if (endBuf > startBuf)
                {
                  // Get the cast but not the arrow, i.e. FOO(bar).
                  std::string str = item->getFormattedAccessor(std::string(startBuf, endBuf - startBuf - 2));

                  if (HandleUnaryOperator(stmt, memberDecl->getType(), str) ||
                      HandleBinaryOperator(stmt, str) ||
                      HandleCompoundAssignOperator(stmt, str))
                    return stmt;

                  if (!item->accessor.empty())
                    ReplaceText(startLoc, endBuf - startBuf + strlen(memberName), str.c_str(), str.size());
                }
              else
                {
                  // Happens when the sourceloc isn't tracked properly so we
                  // can't do any rewrite (currently this happens for nested
                  // macros for example). We could try to directly parse the
                  // text buffer and do some simple rewrites, that could help
                  // with cases like "GTK_BOX (GTK_DIALOG (dialog)->vbox)".
                }

              if (lastStmt && !item->comment.empty())
                {
                  InsertComment(item->getFormattedComment());
                }

              return stmt;
            }
        }
    }

  // Inject any new local variables needed for get-by-reference functions
  if (!newLocals.empty())
    {
      // Remove duplicates
      std::sort(newLocals.begin(), newLocals.end());
      newLocals.erase (std::unique (newLocals.begin(), newLocals.end()), newLocals.end());

      if (!isa<DeclStmt>(stmt) && depth == 0)
        {
          std::vector<LocalReferenceItem>::iterator iter, end;
          std::string newText = "";
          SourceLocation loc;
          bool removeLast = false;

          if (lastLocalDecl == NULL)
            {
              std::string localName;
              Stmt::child_iterator child = stmt->child_begin();
              loc = (*child)->getLocStart();

              for (iter = newLocals.begin(), end = newLocals.end(); iter != end; iter++)
                {
                  localName = CreateUniqueLocalName((*iter).localName, existingLocals);
                  newText += (*iter).refType + " " + localName + ";\n";

                  (*iter).localName = localName;
                }
            }
          else
            {
              std::string localName;
              loc = lastLocalDecl->getLocEnd();

              newText += ";\n";

              for (iter = newLocals.begin(), end = newLocals.end(); iter != end; iter++)
                {
                  localName = CreateUniqueLocalName((*iter).localName, existingLocals);
                  newText += "  " + (*iter).refType + " " + localName + ";\n";

                  (*iter).localName = localName;
                }

              removeLast = true;
            }

          newText += "\n";

          for (iter = newLocals.begin(), end = newLocals.end(); iter != end; iter++)
            {
              // Move this into getFormattedAccessor() probably
              newText += "  " + (*iter).accessor + "(" + (*iter).objName + ", &" + (*iter).localName + ");\n";
            }

          InsertText(loc, newText.c_str(), newText.size());

          if (removeLast)
            {
              RemoveText(loc, 1);  // remove trailing semicolon
            }
	}
    }

  return stmt;
}