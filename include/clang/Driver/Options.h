//===--- Options.h - Option info & table ------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef CLANG_DRIVER_OPTIONS_H_
#define CLANG_DRIVER_OPTIONS_H_

namespace clang {
namespace driver {
namespace options {
  enum ID {
    NotOption = 0, // This is not an option ID.
#define OPTION(ID, KIND, NAME, GROUP, ALIAS, FLAGS, PARAM) ID,
#include "clang/Driver/Options.def"
    LastOption
#undef OPTION
  };
}
  
  class Option;

  /// OptTable - Provide access to the Option info table.
  ///
  /// The OptTable class provides a layer of indirection which allows
  /// Option instance to be created lazily. In the common case, only a
  /// few options will be needed at runtime; the OptTable class
  /// maintains enough information to parse command lines without
  /// instantiating Options, while letting other parts of the driver
  /// still use Option instances where convient.  
  class OptTable {
    mutable Option **Options;

    Option *constructOption(options::ID id) const;

  public:
    OptTable();
    ~OptTable();

    unsigned getNumOptions() const;

    const char *getOptionName(options::ID id) const;

    /// getOption - Get the given \arg id's Option instance, lazily
    /// creating it if necessary.
    const Option *getOption(options::ID id) const;
  };
}
}

#endif