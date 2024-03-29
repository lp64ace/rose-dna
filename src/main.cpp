//===---- tools/extra/ToolTemplate.cpp - Template for refactoring tool ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements an empty refactoring tool using the clang tooling.
//  The goal is to lower the "barrier to entry" for writing refactoring tools.
//
//  Usage:
//  tool-template <cmake-output-dir> <file1> <file2> ...
//
//  Where <cmake-output-dir> is a CMake build directory in which a file named
//  compile_commands.json exists (enable -DCMAKE_EXPORT_COMPILE_COMMANDS in
//  CMake to get this output).
//
//  <file1> ... specify the paths of files in the CMake source tree. This path
//  is looked up in the compile command database. If the path of a file is
//  absolute, it needs to point into CMake's source tree. If the path is
//  relative, the current working directory needs to be in the CMake source
//  tree and the file must be in a subdirectory of the current working
//  directory. "./" prefixes in the relative files will be automatically
//  removed, but the rest of a relative path must be a suffix of a path in
//  the compile command line database.
//
//  For example, to use tool-template on all files in a subtree of the
//  source tree, use:
//
//    /path/in/subtree $ find . -name '*.cpp'|
//        xargs tool-template /path/to/build
//
//===----------------------------------------------------------------------===//

#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Execution.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Refactoring/AtomicChange.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

#include <iostream>

using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;
using namespace llvm;

/** The reason offset, size and array ar integers is because we want to have the
 * same time in both x86 and x64. */

typedef struct DNAField {
  char name[64];
  /** Use with caution this might not exist in SDNA. */
  char type[64];

  int offset;
  int size;
  int align;
  int array;

  int flags;
} DNAField;

enum {
  /** This field is a pointer, if this is an array too the elements of the array
     are pointers. */
  DNA_FIELD_IS_POINTER = (1 << 0),
  /** This field is an array, use the #DNAField->array to get the size of the
     array. */
  DNA_FIELD_IS_ARRAY = (1 << 1),
  /** This field is a pointer to a function (since all structures are in C). */
  DNA_FIELD_IS_FUNCTION = (1 << 2),
};

typedef struct DNAStruct {
  char name[64];

  int size;

  DNAField *_Fields;
  int _FieldsLen;
} DNAStruct;

typedef struct SDNA {
  DNAStruct *_Types;
  int _TypesLen;
} SDNA;

DNAStruct *DNA_add_struct(SDNA *DNA, const std::string &name) {
  size_t alloc = sizeof(DNAStruct) * (DNA->_TypesLen + 1);
  DNAStruct *arr = (DNAStruct *)(realloc(DNA->_Types, alloc));
  if (arr) {
    DNAStruct *Struct = &((DNA->_Types = arr)[DNA->_TypesLen++]);
    memset(Struct, 0, sizeof(DNAStruct));
    strncpy(Struct->name, name.c_str(), sizeof(Struct->name));
    return Struct;
  }
  return NULL;
}

DNAField *DNA_add_field(DNAStruct *Struct, const std::string &name) {
  size_t alloc = sizeof(DNAField) * (Struct->_FieldsLen + 1);
  DNAField *arr = (DNAField *)(realloc(Struct->_Fields, alloc));
  if (arr) {
    DNAField *Field = &((Struct->_Fields = arr)[Struct->_FieldsLen++]);
    memset(Field, 0, sizeof(DNAField));
    strncpy(Field->name, name.c_str(), sizeof(Struct->name));
    return Field;
  }
  return NULL;
}

namespace {
class TypedefDeclCallback : public MatchFinder::MatchCallback {
public:
  TypedefDeclCallback(SDNA *DNA, ExecutionContext &Context)
      : Context(Context), DNA(DNA) {}

  void run(const MatchFinder::MatchResult &Result) override {
    if (auto *TD = Result.Nodes.getNodeAs<clang::TypedefDecl>("typedef")) {
      ASTContext &CTX = TD->getASTContext();

      QualType Qual = TD->getUnderlyingType();
      auto *RD = Qual->getAsRecordDecl();

      if (RD) {
        if (!RD->getBeginLoc().isValid()) {
          /** Clang builtin types are annoying. */
          return;
        }

        DNAStruct *Struct = DNA_add_struct(DNA, Qual.getAsString());

        Struct->size = CTX.getTypeInfo(Qual).Width / 8;

        for (auto *FD : RD->fields()) {
          QualType FieldQual = FD->getType();
          size_t size = CTX.getTypeInfo(FieldQual).Width / 8;
          size_t align = CTX.getTypeInfo(FieldQual).Align / 8;
          size_t offset = CTX.getFieldOffset(FD);

          DNAField *Field = DNA_add_field(Struct, FD->getNameAsString());

          Field->size = size;
          Field->align = align;
          Field->offset = offset;

          /** Conventional so that single items can be multiplied. */
          Field->array = 1;

          if (FieldQual->isPointerType()) {
            Field->flags |= DNA_FIELD_IS_POINTER;
          }
          if (FieldQual->isFunctionPointerType()) {
            Field->flags |= DNA_FIELD_IS_FUNCTION;
          }

          if (FieldQual->isPointerType() || FieldQual->isFunctionPointerType()) {
            /** This should be treated as a pointer. */
            QualType PointeeQual = FieldQual->getPointeeType();
            std::string tp = PointeeQual.getAsString();
            strncpy(Field->type, tp.c_str(), sizeof(Field->type));
          } else if (FieldQual->isArrayType()) {
            /** This should be treated as an array. */

            /** Find the simplest element type of arrays. */
            const clang::ArrayType *AT = FieldQual->getAsArrayTypeUnsafe();
            while (AT->getElementType()->isArrayType()) {
              AT = AT->getElementType()->getAsArrayTypeUnsafe();
            }
            QualType ArrayElementQual = AT->getElementType();
            size_t elem_size = CTX.getTypeInfo(ArrayElementQual).Width / 8;

            Field->array = size / elem_size;

            if (ArrayElementQual->isPointerType()) {
              Field->flags |= DNA_FIELD_IS_POINTER;

              QualType PointeeQual = ArrayElementQual->getPointeeType();
              std::string tp = PointeeQual.getAsString();
              strncpy(Field->type, tp.c_str(), sizeof(Field->type));
            } else {
              std::string tp = ArrayElementQual.getAsString();
              strncpy(Field->type, tp.c_str(), sizeof(Field->type));
            }
          } else {
            /** Treat as a normal buffer of bytes. */
            std::string tp = FieldQual.getAsString();
            strncpy(Field->type, tp.c_str(), sizeof(Field->type));
          }
        }
      }
    }
  }

private:
  ExecutionContext &Context;
  SDNA *DNA;
};
} // end anonymous namespace

// Set up the command line options
static cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static cl::OptionCategory ToolTemplateCategory("rose-dna options");

static cl::opt<std::string>
    DNAOutput("dna", cl::desc(R"(Specify the output file for rose DNA.)"),
             cl::init("clang-rose.dna"), cl::cat(ToolTemplateCategory));

/** Does not include the null terminator */
void WriteWordOut(std::vector<unsigned char> &Buffer, const std::string &Word) {
  unsigned char *raw = (unsigned char *)Word.c_str();
  for (unsigned char *itr = raw; itr != raw + Word.size(); itr++) {
    Buffer.push_back(*itr);
  }
}

/** Does include the null terminator */
void WriteStringOut(std::vector<unsigned char> &Buffer, const std::string &Word) {
  unsigned char *raw = (unsigned char *)Word.c_str();
  for (unsigned char *itr = raw; itr != raw + Word.size(); itr++) {
    Buffer.push_back(*itr);
  }
  Buffer.push_back((unsigned char)'\0');
}

void WriteIntOut(std::vector<unsigned char> &Buffer, int value) {
  unsigned char *raw = (unsigned char *)&value;
  for (unsigned char *itr = raw; itr != raw + sizeof(value); itr++) {
    Buffer.push_back(*itr);
  }
}

int main(int argc, const char **argv) {
  llvm::sys::PrintStackTraceOnErrorSignal(argv[0]);

  auto Executor = clang::tooling::createExecutorFromCommandLineArgs(
      argc, argv, ToolTemplateCategory);

  if (!Executor) {
    llvm::errs() << llvm::toString(Executor.takeError()) << "\n";
    return 1;
  }

  SDNA DNA;
  memset(&DNA, 0, sizeof(SDNA));

  ast_matchers::MatchFinder Finder;
  TypedefDeclCallback Callback(&DNA, *Executor->get()->getExecutionContext());
  Finder.addMatcher(typedefDecl().bind("typedef"), &Callback);

  auto Err = Executor->get()->execute(newFrontendActionFactory(&Finder));
  if (Err) {
    llvm::errs() << llvm::toString(std::move(Err)) << "\n";
  }

  std::vector<unsigned char> _BufferOut;
  /** Can be read as int32, to recognize the endianess. */
  WriteWordOut(_BufferOut, "SDNA");

  WriteIntOut(_BufferOut, DNA._TypesLen);
  for (DNAStruct *Struct = DNA._Types; Struct != DNA._Types + DNA._TypesLen;
       ++Struct) {
    WriteStringOut(_BufferOut, Struct->name);
    WriteIntOut(_BufferOut, Struct->size);

    WriteIntOut(_BufferOut, Struct->_FieldsLen);
    for (DNAField *Field = Struct->_Fields;
         Field != Struct->_Fields + Struct->_FieldsLen; ++Field) {
      WriteStringOut(_BufferOut, Field->name);
      WriteStringOut(_BufferOut, Field->type);
      WriteIntOut(_BufferOut, Field->offset);
      WriteIntOut(_BufferOut, Field->size);
      WriteIntOut(_BufferOut, Field->align);
      WriteIntOut(_BufferOut, Field->array);
      WriteIntOut(_BufferOut, Field->flags);
    }
  }

  std::string DNAFile = DNAOutput.getValue();

  int ExitStatus = 0;
#if defined(WIN32) && WIN32
  FILE *out = fopen(DNAFile.c_str(), "wb");
#else
  FILE *out = fopen(DNAFile.c_str(), "w");
#endif
  if (out) {
    if (fwrite(_BufferOut.data(), 1, _BufferOut.size(), out) !=
        _BufferOut.size()) {
      std::cout << "Failed to write in output DNA file." << std::endl;
      ExitStatus = -2;
    }
    fclose(out);
  } else {
    std::cout << "Failed to open output DNA file." << std::endl;
    ExitStatus = -1;
  }
  return ExitStatus;
}
