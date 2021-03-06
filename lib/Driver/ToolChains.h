//===--- ToolChains.h - ToolChain Implementations ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_LIB_DRIVER_TOOLCHAINS_H_
#define CLANG_LIB_DRIVER_TOOLCHAINS_H_

#include "clang/Driver/Action.h"
#include "clang/Driver/ToolChain.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Compiler.h"

#include "Tools.h"

namespace clang {
namespace driver {
namespace toolchains {

  /// Generic_GCC - A tool chain using the 'gcc' command to perform
  /// all subcommands; this relies on gcc translating the majority of
  /// command line options.
class VISIBILITY_HIDDEN Generic_GCC : public ToolChain {
protected:
  mutable llvm::DenseMap<unsigned, Tool*> Tools;

public:
  Generic_GCC(const HostInfo &Host, const char *Arch, const char *Platform, 
              const char *OS);
  ~Generic_GCC();

  virtual DerivedArgList *TranslateArgs(InputArgList &Args) const;

  virtual Tool &SelectTool(const Compilation &C, const JobAction &JA) const;

  virtual bool IsMathErrnoDefault() const;
  virtual bool IsUnwindTablesDefault() const;
  virtual const char *GetDefaultRelocationModel() const;
  virtual const char *GetForcedPicModel() const;
};

  /// Darwin_X86 - Darwin tool chain for i386 an x86_64.
class VISIBILITY_HIDDEN Darwin_X86 : public ToolChain {
  mutable llvm::DenseMap<unsigned, Tool*> Tools;

  /// Darwin version of tool chain.
  unsigned DarwinVersion[3];

  /// GCC version to use.
  unsigned GCCVersion[3];

  /// The directory suffix for this tool chain.
  std::string ToolChainDir;

  /// The default macosx-version-min of this tool chain; empty until
  /// initialized.
  mutable std::string MacosxVersionMin;

  const char *getMacosxVersionMin() const;

public:
  Darwin_X86(const HostInfo &Host, const char *Arch, const char *Platform, 
             const char *OS, const unsigned (&DarwinVersion)[3],
             const unsigned (&GCCVersion)[3]);
  ~Darwin_X86();

  void getDarwinVersion(unsigned (&Res)[3]) const {
    Res[0] = DarwinVersion[0];
    Res[1] = DarwinVersion[1];
    Res[2] = DarwinVersion[2];
  }

  void getMacosxVersion(unsigned (&Res)[3]) const {
    Res[0] = 10;
    Res[1] = DarwinVersion[0] - 4;
    Res[2] = DarwinVersion[1];
  }

  const char *getMacosxVersionStr() const {
    return MacosxVersionMin.c_str();
  }

  const std::string &getToolChainDir() const { 
    return ToolChainDir;
  }

  virtual DerivedArgList *TranslateArgs(InputArgList &Args) const;

  virtual Tool &SelectTool(const Compilation &C, const JobAction &JA) const;

  virtual bool IsMathErrnoDefault() const;
  virtual bool IsUnwindTablesDefault() const;
  virtual const char *GetDefaultRelocationModel() const;
  virtual const char *GetForcedPicModel() const;
};

  /// Darwin_GCC - Generic Darwin tool chain using gcc.
class VISIBILITY_HIDDEN Darwin_GCC : public Generic_GCC {
public:
  Darwin_GCC(const HostInfo &Host, const char *Arch, const char *Platform, 
             const char *OS) : Generic_GCC(Host, Arch, Platform, OS) {}

  virtual const char *GetDefaultRelocationModel() const { return "pic"; }
};

class VISIBILITY_HIDDEN FreeBSD : public Generic_GCC {
public:
  FreeBSD(const HostInfo &Host, const char *Arch, const char *Platform, 
          const char *OS, bool Lib32);

  virtual Tool &SelectTool(const Compilation &C, const JobAction &JA) const;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif
