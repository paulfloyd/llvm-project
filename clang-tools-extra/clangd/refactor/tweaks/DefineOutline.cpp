//===--- DefineOutline.cpp ---------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "AST.h"
#include "FindTarget.h"
#include "HeaderSourceSwitch.h"
#include "ParsedAST.h"
#include "Selection.h"
#include "SourceCode.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Tooling/Core/Replacement.h"
#include "clang/Tooling/Syntax/Tokens.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Error.h"
#include <cstddef>
#include <optional>
#include <string>

namespace clang {
namespace clangd {
namespace {

// Deduces the FunctionDecl from a selection. Requires either the function body
// or the function decl to be selected. Returns null if none of the above
// criteria is met.
// FIXME: This is shared with define inline, move them to a common header once
// we have a place for such.
const FunctionDecl *getSelectedFunction(const SelectionTree::Node *SelNode) {
  if (!SelNode)
    return nullptr;
  const DynTypedNode &AstNode = SelNode->ASTNode;
  if (const FunctionDecl *FD = AstNode.get<FunctionDecl>())
    return FD;
  if (AstNode.get<CompoundStmt>() &&
      SelNode->Selected == SelectionTree::Complete) {
    if (const SelectionTree::Node *P = SelNode->Parent)
      return P->ASTNode.get<FunctionDecl>();
  }
  return nullptr;
}

std::optional<Path> getSourceFile(llvm::StringRef FileName,
                                  const Tweak::Selection &Sel) {
  assert(Sel.FS);
  if (auto Source = getCorrespondingHeaderOrSource(FileName, Sel.FS))
    return *Source;
  return getCorrespondingHeaderOrSource(FileName, *Sel.AST, Sel.Index);
}

// Synthesize a DeclContext for TargetNS from CurContext. TargetNS must be empty
// for global namespace, and endwith "::" otherwise.
// Returns std::nullopt if TargetNS is not a prefix of CurContext.
std::optional<const DeclContext *>
findContextForNS(llvm::StringRef TargetNS, const DeclContext *CurContext) {
  assert(TargetNS.empty() || TargetNS.ends_with("::"));
  // Skip any non-namespace contexts, e.g. TagDecls, functions/methods.
  CurContext = CurContext->getEnclosingNamespaceContext();
  // If TargetNS is empty, it means global ns, which is translation unit.
  if (TargetNS.empty()) {
    while (!CurContext->isTranslationUnit())
      CurContext = CurContext->getParent();
    return CurContext;
  }
  // Otherwise we need to drop any trailing namespaces from CurContext until
  // we reach TargetNS.
  std::string TargetContextNS =
      CurContext->isNamespace()
          ? llvm::cast<NamespaceDecl>(CurContext)->getQualifiedNameAsString()
          : "";
  TargetContextNS.append("::");

  llvm::StringRef CurrentContextNS(TargetContextNS);
  // If TargetNS is not a prefix of CurrentContext, there's no way to reach
  // it.
  if (!CurrentContextNS.starts_with(TargetNS))
    return std::nullopt;

  while (CurrentContextNS != TargetNS) {
    CurContext = CurContext->getParent();
    // These colons always exists since TargetNS is a prefix of
    // CurrentContextNS, it ends with "::" and they are not equal.
    CurrentContextNS = CurrentContextNS.take_front(
        CurrentContextNS.drop_back(2).rfind("::") + 2);
  }
  return CurContext;
}

// Returns source code for FD after applying Replacements.
// FIXME: Make the function take a parameter to return only the function body,
// afterwards it can be shared with define-inline code action.
llvm::Expected<std::string>
getFunctionSourceAfterReplacements(const FunctionDecl *FD,
                                   const tooling::Replacements &Replacements,
                                   bool TargetFileIsHeader) {
  const auto &SM = FD->getASTContext().getSourceManager();
  auto OrigFuncRange = toHalfOpenFileRange(
      SM, FD->getASTContext().getLangOpts(), FD->getSourceRange());
  if (!OrigFuncRange)
    return error("Couldn't get range for function.");

  // Get new begin and end positions for the qualified function definition.
  unsigned FuncBegin = SM.getFileOffset(OrigFuncRange->getBegin());
  unsigned FuncEnd = Replacements.getShiftedCodePosition(
      SM.getFileOffset(OrigFuncRange->getEnd()));

  // Trim the result to function definition.
  auto QualifiedFunc = tooling::applyAllReplacements(
      SM.getBufferData(SM.getMainFileID()), Replacements);
  if (!QualifiedFunc)
    return QualifiedFunc.takeError();

  auto Source = QualifiedFunc->substr(FuncBegin, FuncEnd - FuncBegin + 1);
  std::string TemplatePrefix;
  auto AddToTemplatePrefixIfApplicable = [&](const Decl *D) {
    const TemplateParameterList *Params = D->getDescribedTemplateParams();
    if (!Params)
      return;
    for (Decl *P : *Params) {
      if (auto *TTP = dyn_cast<TemplateTypeParmDecl>(P))
        TTP->removeDefaultArgument();
      else if (auto *NTTP = dyn_cast<NonTypeTemplateParmDecl>(P))
        NTTP->removeDefaultArgument();
      else if (auto *TTPD = dyn_cast<TemplateTemplateParmDecl>(P))
        TTPD->removeDefaultArgument();
    }
    std::string S;
    llvm::raw_string_ostream Stream(S);
    Params->print(Stream, FD->getASTContext());
    if (!S.empty())
      *S.rbegin() = '\n'; // Replace space with newline
    TemplatePrefix.insert(0, S);
  };
  AddToTemplatePrefixIfApplicable(FD);
  if (auto *MD = llvm::dyn_cast<CXXMethodDecl>(FD)) {
    for (const CXXRecordDecl *Parent = MD->getParent(); Parent;
         Parent =
             llvm::dyn_cast_or_null<const CXXRecordDecl>(Parent->getParent())) {
      AddToTemplatePrefixIfApplicable(Parent);
    }
  }

  if (TargetFileIsHeader)
    Source.insert(0, "inline ");
  if (!TemplatePrefix.empty())
    Source.insert(0, TemplatePrefix);
  return Source;
}

// Returns replacements to delete tokens with kind `Kind` in the range
// `FromRange`. Removes matching instances of given token preceeding the
// function defition.
llvm::Expected<tooling::Replacements>
deleteTokensWithKind(const syntax::TokenBuffer &TokBuf, tok::TokenKind Kind,
                     SourceRange FromRange) {
  tooling::Replacements DelKeywordCleanups;
  llvm::Error Errors = llvm::Error::success();
  bool FoundAny = false;
  for (const auto &Tok : TokBuf.expandedTokens(FromRange)) {
    if (Tok.kind() != Kind)
      continue;
    FoundAny = true;
    auto Spelling = TokBuf.spelledForExpanded(llvm::ArrayRef(Tok));
    if (!Spelling) {
      Errors = llvm::joinErrors(
          std::move(Errors),
          error("define outline: couldn't remove `{0}` keyword.",
                tok::getKeywordSpelling(Kind)));
      break;
    }
    auto &SM = TokBuf.sourceManager();
    CharSourceRange DelRange =
        syntax::Token::range(SM, Spelling->front(), Spelling->back())
            .toCharRange(SM);
    if (auto Err =
            DelKeywordCleanups.add(tooling::Replacement(SM, DelRange, "")))
      Errors = llvm::joinErrors(std::move(Errors), std::move(Err));
  }
  if (!FoundAny) {
    Errors = llvm::joinErrors(
        std::move(Errors),
        error("define outline: couldn't find `{0}` keyword to remove.",
              tok::getKeywordSpelling(Kind)));
  }

  if (Errors)
    return std::move(Errors);
  return DelKeywordCleanups;
}

// Creates a modified version of function definition that can be inserted at a
// different location, qualifies return value and function name to achieve that.
// Contains function signature, except defaulted parameter arguments, body and
// template parameters if applicable. No need to qualify parameters, as they are
// looked up in the context containing the function/method.
// FIXME: Drop attributes in function signature.
llvm::Expected<std::string>
getFunctionSourceCode(const FunctionDecl *FD, const DeclContext *TargetContext,
                      const syntax::TokenBuffer &TokBuf,
                      const HeuristicResolver *Resolver,
                      bool TargetFileIsHeader) {
  auto &AST = FD->getASTContext();
  auto &SM = AST.getSourceManager();

  llvm::Error Errors = llvm::Error::success();
  tooling::Replacements DeclarationCleanups;

  // Finds the first unqualified name in function return type and name, then
  // qualifies those to be valid in TargetContext.
  findExplicitReferences(
      FD,
      [&](ReferenceLoc Ref) {
        // It is enough to qualify the first qualifier, so skip references with
        // a qualifier. Also we can't do much if there are no targets or name is
        // inside a macro body.
        if (Ref.Qualifier || Ref.Targets.empty() || Ref.NameLoc.isMacroID())
          return;
        // Only qualify return type and function name.
        if (Ref.NameLoc != FD->getReturnTypeSourceRange().getBegin() &&
            Ref.NameLoc != FD->getLocation())
          return;

        for (const NamedDecl *ND : Ref.Targets) {
          if (ND->getKind() == Decl::TemplateTypeParm)
            return;
          if (ND->getDeclContext() != Ref.Targets.front()->getDeclContext()) {
            elog("Targets from multiple contexts: {0}, {1}",
                 printQualifiedName(*Ref.Targets.front()),
                 printQualifiedName(*ND));
            return;
          }
        }
        const NamedDecl *ND = Ref.Targets.front();
        std::string Qualifier =
            getQualification(AST, TargetContext,
                             SM.getLocForStartOfFile(SM.getMainFileID()), ND);
        if (ND->getDeclContext()->isDependentContext() &&
            llvm::isa<TypeDecl>(ND)) {
          Qualifier.insert(0, "typename ");
        }
        if (auto Err = DeclarationCleanups.add(
                tooling::Replacement(SM, Ref.NameLoc, 0, Qualifier)))
          Errors = llvm::joinErrors(std::move(Errors), std::move(Err));
      },
      Resolver);

  // findExplicitReferences doesn't provide references to
  // constructor/destructors, it only provides references to type names inside
  // them.
  // this works for constructors, but doesn't work for destructor as type name
  // doesn't cover leading `~`, so handle it specially.
  if (const auto *Destructor = llvm::dyn_cast<CXXDestructorDecl>(FD)) {
    if (auto Err = DeclarationCleanups.add(tooling::Replacement(
            SM, Destructor->getLocation(), 0,
            getQualification(AST, TargetContext,
                             SM.getLocForStartOfFile(SM.getMainFileID()),
                             Destructor))))
      Errors = llvm::joinErrors(std::move(Errors), std::move(Err));
  }

  // Get rid of default arguments, since they should not be specified in
  // out-of-line definition.
  for (const auto *PVD : FD->parameters()) {
    if (!PVD->hasDefaultArg())
      continue;
    // Deletion range spans the initializer, usually excluding the `=`.
    auto DelRange = CharSourceRange::getTokenRange(PVD->getDefaultArgRange());
    // Get all tokens before the default argument.
    auto Tokens = TokBuf.expandedTokens(PVD->getSourceRange())
                      .take_while([&SM, &DelRange](const syntax::Token &Tok) {
                        return SM.isBeforeInTranslationUnit(
                            Tok.location(), DelRange.getBegin());
                      });
    if (TokBuf.expandedTokens(DelRange.getAsRange()).front().kind() !=
        tok::equal) {
      // Find the last `=` if it isn't included in the initializer, and update
      // the DelRange to include it.
      auto Tok =
          llvm::find_if(llvm::reverse(Tokens), [](const syntax::Token &Tok) {
            return Tok.kind() == tok::equal;
          });
      assert(Tok != Tokens.rend());
      DelRange.setBegin(Tok->location());
    }
    if (auto Err =
            DeclarationCleanups.add(tooling::Replacement(SM, DelRange, "")))
      Errors = llvm::joinErrors(std::move(Errors), std::move(Err));
  }

  auto DelAttr = [&](const Attr *A) {
    if (!A)
      return;
    auto AttrTokens =
        TokBuf.spelledForExpanded(TokBuf.expandedTokens(A->getRange()));
    assert(A->getLocation().isValid());
    if (!AttrTokens || AttrTokens->empty()) {
      Errors = llvm::joinErrors(
          std::move(Errors), error("define outline: Can't move out of line as "
                                   "function has a macro `{0}` specifier.",
                                   A->getSpelling()));
      return;
    }
    CharSourceRange DelRange =
        syntax::Token::range(SM, AttrTokens->front(), AttrTokens->back())
            .toCharRange(SM);
    if (auto Err =
            DeclarationCleanups.add(tooling::Replacement(SM, DelRange, "")))
      Errors = llvm::joinErrors(std::move(Errors), std::move(Err));
  };

  DelAttr(FD->getAttr<OverrideAttr>());
  DelAttr(FD->getAttr<FinalAttr>());

  auto DelKeyword = [&](tok::TokenKind Kind, SourceRange FromRange) {
    auto DelKeywords = deleteTokensWithKind(TokBuf, Kind, FromRange);
    if (!DelKeywords) {
      Errors = llvm::joinErrors(std::move(Errors), DelKeywords.takeError());
      return;
    }
    DeclarationCleanups = DeclarationCleanups.merge(*DelKeywords);
  };

  if (FD->isInlineSpecified())
    DelKeyword(tok::kw_inline, {FD->getBeginLoc(), FD->getLocation()});
  if (const auto *MD = dyn_cast<CXXMethodDecl>(FD)) {
    if (MD->isVirtualAsWritten())
      DelKeyword(tok::kw_virtual, {FD->getBeginLoc(), FD->getLocation()});
    if (MD->isStatic())
      DelKeyword(tok::kw_static, {FD->getBeginLoc(), FD->getLocation()});
  }
  if (const auto *CD = dyn_cast<CXXConstructorDecl>(FD)) {
    if (CD->isExplicit())
      DelKeyword(tok::kw_explicit, {FD->getBeginLoc(), FD->getLocation()});
  }

  if (Errors)
    return std::move(Errors);
  return getFunctionSourceAfterReplacements(FD, DeclarationCleanups,
                                            TargetFileIsHeader);
}

struct InsertionPoint {
  const DeclContext *EnclosingNamespace = nullptr;
  size_t Offset;
};

// Returns the range that should be deleted from declaration, which always
// contains function body. In addition to that it might contain constructor
// initializers.
SourceRange getDeletionRange(const FunctionDecl *FD,
                             const syntax::TokenBuffer &TokBuf) {
  auto DeletionRange = FD->getBody()->getSourceRange();
  if (auto *CD = llvm::dyn_cast<CXXConstructorDecl>(FD)) {
    // AST doesn't contain the location for ":" in ctor initializers. Therefore
    // we find it by finding the first ":" before the first ctor initializer.
    SourceLocation InitStart;
    // Find the first initializer.
    for (const auto *CInit : CD->inits()) {
      // SourceOrder is -1 for implicit initializers.
      if (CInit->getSourceOrder() != 0)
        continue;
      InitStart = CInit->getSourceLocation();
      break;
    }
    if (InitStart.isValid()) {
      auto Toks = TokBuf.expandedTokens(CD->getSourceRange());
      // Drop any tokens after the initializer.
      Toks = Toks.take_while([&TokBuf, &InitStart](const syntax::Token &Tok) {
        return TokBuf.sourceManager().isBeforeInTranslationUnit(Tok.location(),
                                                                InitStart);
      });
      // Look for the first colon.
      auto Tok =
          llvm::find_if(llvm::reverse(Toks), [](const syntax::Token &Tok) {
            return Tok.kind() == tok::colon;
          });
      assert(Tok != Toks.rend());
      DeletionRange.setBegin(Tok->location());
    }
  }
  return DeletionRange;
}

/// Moves definition of a function/method to an appropriate implementation file.
///
/// Before:
/// a.h
///   void foo() { return; }
/// a.cc
///   #include "a.h"
///
/// ----------------
///
/// After:
/// a.h
///   void foo();
/// a.cc
///   #include "a.h"
///   void foo() { return; }
class DefineOutline : public Tweak {
public:
  const char *id() const override;

  bool hidden() const override { return false; }
  llvm::StringLiteral kind() const override {
    return CodeAction::REFACTOR_KIND;
  }
  std::string title() const override {
    return "Move function body to out-of-line";
  }

  bool prepare(const Selection &Sel) override {
    SameFile = !isHeaderFile(Sel.AST->tuPath(), Sel.AST->getLangOpts());
    Source = getSelectedFunction(Sel.ASTSelection.commonAncestor());

    // Bail out if the selection is not a in-line function definition.
    if (!Source || !Source->doesThisDeclarationHaveABody() ||
        Source->isOutOfLine())
      return false;

    // Bail out if this is a function template specialization, as their
    // definitions need to be visible in all including translation units.
    if (Source->getTemplateSpecializationInfo())
      return false;

    auto *MD = llvm::dyn_cast<CXXMethodDecl>(Source);
    if (!MD) {
      if (Source->getDescribedFunctionTemplate())
        return false;
      // Can't outline free-standing functions in the same file.
      return !SameFile;
    }

    for (const CXXRecordDecl *Parent = MD->getParent(); Parent;
         Parent =
             llvm::dyn_cast_or_null<const CXXRecordDecl>(Parent->getParent())) {
      if (const TemplateParameterList *Params =
              Parent->getDescribedTemplateParams()) {

        // Class template member functions must be defined in the
        // same file.
        SameFile = true;

        // Bail out if the template parameter is unnamed.
        for (NamedDecl *P : *Params) {
          if (!P->getIdentifier())
            return false;
        }
      }
    }

    // Function templates must be defined in the same file.
    if (MD->getDescribedTemplate())
      SameFile = true;

    // The refactoring is meaningless for unnamed classes and namespaces,
    // unless we're outlining in the same file
    for (const DeclContext *DC = MD->getParent(); DC; DC = DC->getParent()) {
      if (auto *ND = llvm::dyn_cast<NamedDecl>(DC)) {
        if (ND->getDeclName().isEmpty() &&
            (!SameFile || !llvm::dyn_cast<NamespaceDecl>(ND)))
          return false;
      }
    }

    // Note that we don't check whether an implementation file exists or not in
    // the prepare, since performing disk IO on each prepare request might be
    // expensive.
    return true;
  }

  Expected<Effect> apply(const Selection &Sel) override {
    const SourceManager &SM = Sel.AST->getSourceManager();
    auto CCFile = SameFile ? Sel.AST->tuPath().str()
                           : getSourceFile(Sel.AST->tuPath(), Sel);
    if (!CCFile)
      return error("Couldn't find a suitable implementation file.");
    assert(Sel.FS && "FS Must be set in apply");
    auto Buffer = Sel.FS->getBufferForFile(*CCFile);
    // FIXME: Maybe we should consider creating the implementation file if it
    // doesn't exist?
    if (!Buffer)
      return llvm::errorCodeToError(Buffer.getError());
    auto Contents = Buffer->get()->getBuffer();
    auto InsertionPoint = getInsertionPoint(Contents, Sel);
    if (!InsertionPoint)
      return InsertionPoint.takeError();

    auto FuncDef = getFunctionSourceCode(
        Source, InsertionPoint->EnclosingNamespace, Sel.AST->getTokens(),
        Sel.AST->getHeuristicResolver(),
        SameFile && isHeaderFile(Sel.AST->tuPath(), Sel.AST->getLangOpts()));
    if (!FuncDef)
      return FuncDef.takeError();

    SourceManagerForFile SMFF(*CCFile, Contents);
    const tooling::Replacement InsertFunctionDef(
        *CCFile, InsertionPoint->Offset, 0, *FuncDef);
    auto Effect = Effect::mainFileEdit(
        SMFF.get(), tooling::Replacements(InsertFunctionDef));
    if (!Effect)
      return Effect.takeError();

    tooling::Replacements HeaderUpdates(tooling::Replacement(
        Sel.AST->getSourceManager(),
        CharSourceRange::getTokenRange(*toHalfOpenFileRange(
            SM, Sel.AST->getLangOpts(),
            getDeletionRange(Source, Sel.AST->getTokens()))),
        ";"));

    if (Source->isInlineSpecified()) {
      auto DelInline =
          deleteTokensWithKind(Sel.AST->getTokens(), tok::kw_inline,
                               {Source->getBeginLoc(), Source->getLocation()});
      if (!DelInline)
        return DelInline.takeError();
      HeaderUpdates = HeaderUpdates.merge(*DelInline);
    }

    if (SameFile) {
      tooling::Replacements &R = Effect->ApplyEdits[*CCFile].Replacements;
      R = R.merge(HeaderUpdates);
    } else {
      auto HeaderFE = Effect::fileEdit(SM, SM.getMainFileID(), HeaderUpdates);
      if (!HeaderFE)
        return HeaderFE.takeError();
      Effect->ApplyEdits.try_emplace(HeaderFE->first,
                                     std::move(HeaderFE->second));
    }
    return std::move(*Effect);
  }

  // Returns the most natural insertion point for \p QualifiedName in \p
  // Contents. This currently cares about only the namespace proximity, but in
  // feature it should also try to follow ordering of declarations. For example,
  // if decls come in order `foo, bar, baz` then this function should return
  // some point between foo and baz for inserting bar.
  // FIXME: The selection can be made smarter by looking at the definition
  // locations for adjacent decls to Source. Unfortunately pseudo parsing in
  // getEligibleRegions only knows about namespace begin/end events so we
  // can't match function start/end positions yet.
  llvm::Expected<InsertionPoint> getInsertionPoint(llvm::StringRef Contents,
                                                   const Selection &Sel) {
    // If the definition goes to the same file and there is a namespace,
    // we should (and, in the case of anonymous namespaces, need to)
    // put the definition into the original namespace block.
    if (SameFile) {
      auto *Klass = Source->getDeclContext()->getOuterLexicalRecordContext();
      if (!Klass)
        return error("moving to same file not supported for free functions");
      const SourceLocation EndLoc = Klass->getBraceRange().getEnd();
      const auto &TokBuf = Sel.AST->getTokens();
      auto Tokens = TokBuf.expandedTokens();
      auto It = llvm::lower_bound(
          Tokens, EndLoc, [](const syntax::Token &Tok, SourceLocation EndLoc) {
            return Tok.location() < EndLoc;
          });
      while (It != Tokens.end()) {
        if (It->kind() != tok::semi) {
          ++It;
          continue;
        }
        unsigned Offset = Sel.AST->getSourceManager()
                              .getDecomposedLoc(It->endLocation())
                              .second;
        return InsertionPoint{Klass->getEnclosingNamespaceContext(), Offset};
      }
      return error(
          "failed to determine insertion location: no end of class found");
    }

    auto Region = getEligiblePoints(
        Contents, Source->getQualifiedNameAsString(), Sel.AST->getLangOpts());

    assert(!Region.EligiblePoints.empty());
    auto Offset = positionToOffset(Contents, Region.EligiblePoints.back());
    if (!Offset)
      return Offset.takeError();

    auto TargetContext =
        findContextForNS(Region.EnclosingNamespace, Source->getDeclContext());
    if (!TargetContext)
      return error("define outline: couldn't find a context for target");

    return InsertionPoint{*TargetContext, *Offset};
  }

private:
  const FunctionDecl *Source = nullptr;
  bool SameFile = false;
};

REGISTER_TWEAK(DefineOutline)

} // namespace
} // namespace clangd
} // namespace clang
