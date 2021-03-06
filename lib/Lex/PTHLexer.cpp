//===--- PTHLexer.cpp - Lex from a token stream ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the PTHLexer interface.
//
//===----------------------------------------------------------------------===//

#include "clang/Basic/TokenKinds.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Lex/PTHLexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PTHManager.h"
#include "clang/Lex/Token.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/System/Host.h"
#include <sys/stat.h>
using namespace clang;

#define DISK_TOKEN_SIZE (1+1+2+4+4)

//===----------------------------------------------------------------------===//
// Utility methods for reading from the mmap'ed PTH file.
//===----------------------------------------------------------------------===//

static inline uint16_t ReadUnalignedLE16(const unsigned char *&Data) {
  uint16_t V = ((uint16_t)Data[0]) |
               ((uint16_t)Data[1] <<  8);
  Data += 2;
  return V;
}

static inline uint32_t ReadUnalignedLE32(const unsigned char *&Data) {
  uint32_t V = ((uint32_t)Data[0])  |
               ((uint32_t)Data[1] << 8)  |
               ((uint32_t)Data[2] << 16) |
               ((uint32_t)Data[3] << 24);
  Data += 4;
  return V;
}

static inline uint64_t ReadUnalignedLE64(const unsigned char *&Data) {
  uint64_t V = ((uint64_t)Data[0])  |
    ((uint64_t)Data[1] << 8)  |
    ((uint64_t)Data[2] << 16) |
    ((uint64_t)Data[3] << 24) |
    ((uint64_t)Data[4] << 32) |
    ((uint64_t)Data[5] << 40) |
    ((uint64_t)Data[6] << 48) |
    ((uint64_t)Data[7] << 56);
  Data += 8;
  return V;
}

static inline uint32_t ReadLE32(const unsigned char *&Data) {
  // Hosts that directly support little-endian 32-bit loads can just
  // use them.  Big-endian hosts need a bswap.
  uint32_t V = *((uint32_t*)Data);
  if (llvm::sys::isBigEndianHost())
    V = llvm::ByteSwap_32(V);
  Data += 4;
  return V;
}

// Bernstein hash function:
// This is basically copy-and-paste from StringMap.  This likely won't
// stay here, which is why I didn't both to expose this function from
// String Map.
static unsigned BernsteinHash(const char* x) {
  unsigned int R = 0;
  for ( ; *x != '\0' ; ++x) R = R * 33 + *x;
  return R + (R >> 5);
}

static unsigned BernsteinHash(const char* x, unsigned n) {
  unsigned int R = 0;
  for (unsigned i = 0 ; i < n ; ++i, ++x) R = R * 33 + *x;
  return R + (R >> 5);
}

//===----------------------------------------------------------------------===//
// PTHLexer methods.
//===----------------------------------------------------------------------===//

PTHLexer::PTHLexer(Preprocessor &PP, FileID FID, const unsigned char *D,
                   const unsigned char *ppcond, PTHManager &PM)
  : PreprocessorLexer(&PP, FID), TokBuf(D), CurPtr(D), LastHashTokPtr(0),
    PPCond(ppcond), CurPPCondPtr(ppcond), PTHMgr(PM) {
      
  FileStartLoc = PP.getSourceManager().getLocForStartOfFile(FID);
}

void PTHLexer::Lex(Token& Tok) {
LexNextToken:

  //===--------------------------------------==//
  // Read the raw token data.
  //===--------------------------------------==//
  
  // Shadow CurPtr into an automatic variable.
  const unsigned char *CurPtrShadow = CurPtr;  

  // Read in the data for the token.
  unsigned Word0 = ReadLE32(CurPtrShadow);
  uint32_t IdentifierID = ReadLE32(CurPtrShadow);
  uint32_t FileOffset = ReadLE32(CurPtrShadow);
  
  tok::TokenKind TKind = (tok::TokenKind) (Word0 & 0xFF);
  Token::TokenFlags TFlags = (Token::TokenFlags) ((Word0 >> 8) & 0xFF);
  uint32_t Len = Word0 >> 16;

  CurPtr = CurPtrShadow;
  
  //===--------------------------------------==//
  // Construct the token itself.
  //===--------------------------------------==//
  
  Tok.startToken();
  Tok.setKind(TKind);
  Tok.setFlag(TFlags);
  assert(!LexingRawMode);
  Tok.setLocation(FileStartLoc.getFileLocWithOffset(FileOffset));
  Tok.setLength(Len);

  // Handle identifiers.
  if (Tok.isLiteral()) {
    Tok.setLiteralData((const char*) (PTHMgr.SpellingBase + IdentifierID));
  }
  else if (IdentifierID) {
    MIOpt.ReadToken();
    IdentifierInfo *II = PTHMgr.GetIdentifierInfo(IdentifierID-1);
    
    Tok.setIdentifierInfo(II);
    
    // Change the kind of this identifier to the appropriate token kind, e.g.
    // turning "for" into a keyword.
    Tok.setKind(II->getTokenID());
    
    if (II->isHandleIdentifierCase())
      PP->HandleIdentifier(Tok);
    return;
  }
  
  //===--------------------------------------==//
  // Process the token.
  //===--------------------------------------==//
#if 0  
  SourceManager& SM = PP->getSourceManager();
  llvm::cerr << SM.getFileEntryForID(FileID)->getName()
    << ':' << SM.getLogicalLineNumber(Tok.getLocation())
    << ':' << SM.getLogicalColumnNumber(Tok.getLocation())
    << '\n';
#endif  

  if (TKind == tok::eof) {
    // Save the end-of-file token.
    EofToken = Tok;
    
    Preprocessor *PPCache = PP;
    
    assert(!ParsingPreprocessorDirective);
    assert(!LexingRawMode);
    
    // FIXME: Issue diagnostics similar to Lexer.
    if (PP->HandleEndOfFile(Tok, false))
      return;
    
    assert(PPCache && "Raw buffer::LexEndOfFile should return a token");
    return PPCache->Lex(Tok);
  }
  
  if (TKind == tok::hash && Tok.isAtStartOfLine()) {
    LastHashTokPtr = CurPtr - DISK_TOKEN_SIZE;
    assert(!LexingRawMode);
    PP->HandleDirective(Tok);
    
    if (PP->isCurrentLexer(this))
      goto LexNextToken;
    
    return PP->Lex(Tok);
  }
  
  if (TKind == tok::eom) {
    assert(ParsingPreprocessorDirective);
    ParsingPreprocessorDirective = false;
    return;
  }

  MIOpt.ReadToken();
}

// FIXME: We can just grab the last token instead of storing a copy
// into EofToken.
void PTHLexer::getEOF(Token& Tok) {
  assert(EofToken.is(tok::eof));
  Tok = EofToken;
}

void PTHLexer::DiscardToEndOfLine() {
  assert(ParsingPreprocessorDirective && ParsingFilename == false &&
         "Must be in a preprocessing directive!");

  // We assume that if the preprocessor wishes to discard to the end of
  // the line that it also means to end the current preprocessor directive.
  ParsingPreprocessorDirective = false;
  
  // Skip tokens by only peeking at their token kind and the flags.
  // We don't need to actually reconstruct full tokens from the token buffer.
  // This saves some copies and it also reduces IdentifierInfo* lookup.
  const unsigned char* p = CurPtr;
  while (1) {
    // Read the token kind.  Are we at the end of the file?
    tok::TokenKind x = (tok::TokenKind) (uint8_t) *p;
    if (x == tok::eof) break;
    
    // Read the token flags.  Are we at the start of the next line?
    Token::TokenFlags y = (Token::TokenFlags) (uint8_t) p[1];
    if (y & Token::StartOfLine) break;

    // Skip to the next token.
    p += DISK_TOKEN_SIZE;
  }
  
  CurPtr = p;
}

/// SkipBlock - Used by Preprocessor to skip the current conditional block.
bool PTHLexer::SkipBlock() {
  assert(CurPPCondPtr && "No cached PP conditional information.");
  assert(LastHashTokPtr && "No known '#' token.");
  
  const unsigned char* HashEntryI = 0;
  uint32_t Offset; 
  uint32_t TableIdx;
  
  do {
    // Read the token offset from the side-table.
    Offset = ReadLE32(CurPPCondPtr);
    
    // Read the target table index from the side-table.    
    TableIdx = ReadLE32(CurPPCondPtr);
    
    // Compute the actual memory address of the '#' token data for this entry.
    HashEntryI = TokBuf + Offset;

    // Optmization: "Sibling jumping".  #if...#else...#endif blocks can
    //  contain nested blocks.  In the side-table we can jump over these
    //  nested blocks instead of doing a linear search if the next "sibling"
    //  entry is not at a location greater than LastHashTokPtr.
    if (HashEntryI < LastHashTokPtr && TableIdx) {
      // In the side-table we are still at an entry for a '#' token that
      // is earlier than the last one we saw.  Check if the location we would
      // stride gets us closer.
      const unsigned char* NextPPCondPtr =
        PPCond + TableIdx*(sizeof(uint32_t)*2);
      assert(NextPPCondPtr >= CurPPCondPtr);
      // Read where we should jump to.
      uint32_t TmpOffset = ReadLE32(NextPPCondPtr);
      const unsigned char* HashEntryJ = TokBuf + TmpOffset;
      
      if (HashEntryJ <= LastHashTokPtr) {
        // Jump directly to the next entry in the side table.
        HashEntryI = HashEntryJ;
        Offset = TmpOffset;
        TableIdx = ReadLE32(NextPPCondPtr);
        CurPPCondPtr = NextPPCondPtr;
      }
    }
  }
  while (HashEntryI < LastHashTokPtr);  
  assert(HashEntryI == LastHashTokPtr && "No PP-cond entry found for '#'");
  assert(TableIdx && "No jumping from #endifs.");
  
  // Update our side-table iterator.
  const unsigned char* NextPPCondPtr = PPCond + TableIdx*(sizeof(uint32_t)*2);
  assert(NextPPCondPtr >= CurPPCondPtr);
  CurPPCondPtr = NextPPCondPtr;
  
  // Read where we should jump to.
  HashEntryI = TokBuf + ReadLE32(NextPPCondPtr);
  uint32_t NextIdx = ReadLE32(NextPPCondPtr);
  
  // By construction NextIdx will be zero if this is a #endif.  This is useful
  // to know to obviate lexing another token.
  bool isEndif = NextIdx == 0;
  
  // This case can occur when we see something like this:
  //
  //  #if ...
  //   /* a comment or nothing */
  //  #elif
  //
  // If we are skipping the first #if block it will be the case that CurPtr
  // already points 'elif'.  Just return.
  
  if (CurPtr > HashEntryI) {
    assert(CurPtr == HashEntryI + DISK_TOKEN_SIZE);
    // Did we reach a #endif?  If so, go ahead and consume that token as well.
    if (isEndif)
      CurPtr += DISK_TOKEN_SIZE*2;
    else
      LastHashTokPtr = HashEntryI;
    
    return isEndif;
  }

  // Otherwise, we need to advance.  Update CurPtr to point to the '#' token.
  CurPtr = HashEntryI;
  
  // Update the location of the last observed '#'.  This is useful if we
  // are skipping multiple blocks.
  LastHashTokPtr = CurPtr;

  // Skip the '#' token.
  assert(((tok::TokenKind)*CurPtr) == tok::hash);
  CurPtr += DISK_TOKEN_SIZE;
  
  // Did we reach a #endif?  If so, go ahead and consume that token as well.
  if (isEndif) { CurPtr += DISK_TOKEN_SIZE*2; }

  return isEndif;
}

SourceLocation PTHLexer::getSourceLocation() {
  // getSourceLocation is not on the hot path.  It is used to get the location
  // of the next token when transitioning back to this lexer when done
  // handling a #included file.  Just read the necessary data from the token
  // data buffer to construct the SourceLocation object.
  // NOTE: This is a virtual function; hence it is defined out-of-line.
  const unsigned char *OffsetPtr = CurPtr + (DISK_TOKEN_SIZE - 4);
  uint32_t Offset = ReadLE32(OffsetPtr);
  return FileStartLoc.getFileLocWithOffset(Offset);
}

//===----------------------------------------------------------------------===//
// OnDiskChainedHashTable
//===----------------------------------------------------------------------===//

template<typename Info>
class OnDiskChainedHashTable {
  const unsigned NumBuckets;
  const unsigned NumEntries;
  const unsigned char* const Buckets;
  const unsigned char* const Base;
public:
  typedef typename Info::internal_key_type internal_key_type;
  typedef typename Info::external_key_type external_key_type;
  typedef typename Info::data_type         data_type;
  
  OnDiskChainedHashTable(unsigned numBuckets, unsigned numEntries,
                         const unsigned char* buckets,
                         const unsigned char* base)
    : NumBuckets(numBuckets), NumEntries(numEntries),
      Buckets(buckets), Base(base) {        
        assert((reinterpret_cast<uintptr_t>(buckets) & 0x3) == 0 &&
               "'buckets' must have a 4-byte alignment");
      }

  unsigned getNumBuckets() const { return NumBuckets; }
  unsigned getNumEntries() const { return NumEntries; }
  const unsigned char* getBase() const { return Base; }
  const unsigned char* getBuckets() const { return Buckets; }

  bool isEmpty() const { return NumEntries == 0; }
  
  class iterator {
    internal_key_type key;
    const unsigned char* const data;
    const unsigned len;
  public:
    iterator() : data(0), len(0) {}
    iterator(const internal_key_type k, const unsigned char* d, unsigned l)
      : key(k), data(d), len(l) {}
    
    data_type operator*() const { return Info::ReadData(key, data, len); }    
    bool operator==(const iterator& X) const { return X.data == data; }    
    bool operator!=(const iterator& X) const { return X.data != data; }
  };    
  
  iterator find(const external_key_type& eKey) {
    const internal_key_type& iKey = Info::GetInternalKey(eKey);
    unsigned key_hash = Info::ComputeHash(iKey);
    
    // Each bucket is just a 32-bit offset into the PTH file.
    unsigned idx = key_hash & (NumBuckets - 1);
    const unsigned char* Bucket = Buckets + sizeof(uint32_t)*idx;
    
    unsigned offset = ReadLE32(Bucket);
    if (offset == 0) return iterator(); // Empty bucket.
    const unsigned char* Items = Base + offset;
    
    // 'Items' starts with a 16-bit unsigned integer representing the
    // number of items in this bucket.
    unsigned len = ReadUnalignedLE16(Items);
    
    for (unsigned i = 0; i < len; ++i) {
      // Read the hash.
      uint32_t item_hash = ReadUnalignedLE32(Items);
      
      // Determine the length of the key and the data.
      const std::pair<unsigned, unsigned>& L = Info::ReadKeyDataLength(Items);      
      unsigned item_len = L.first + L.second;

      // Compare the hashes.  If they are not the same, skip the entry entirely.
      if (item_hash != key_hash) {
        Items += item_len;
        continue;
      }
      
      // Read the key.
      const internal_key_type& X =
        Info::ReadKey((const unsigned char* const) Items, L.first);

      // If the key doesn't match just skip reading the value.
      if (!Info::EqualKey(X, iKey)) {
        Items += item_len;
        continue;
      }
      
      // The key matches!
      return iterator(X, Items + L.first, L.second);
    }
    
    return iterator();
  }
  
  iterator end() const { return iterator(); }
  
  
  static OnDiskChainedHashTable* Create(const unsigned char* buckets,
                                        const unsigned char* const base) {

    assert(buckets > base);
    assert((reinterpret_cast<uintptr_t>(buckets) & 0x3) == 0 &&
           "buckets should be 4-byte aligned.");
    
    unsigned numBuckets = ReadLE32(buckets);
    unsigned numEntries = ReadLE32(buckets);
    return new OnDiskChainedHashTable<Info>(numBuckets, numEntries, buckets,
                                            base);
  }  
};

//===----------------------------------------------------------------------===//
// PTH file lookup: map from strings to file data.
//===----------------------------------------------------------------------===//

/// PTHFileLookup - This internal data structure is used by the PTHManager
///  to map from FileEntry objects managed by FileManager to offsets within
///  the PTH file.
namespace {
class VISIBILITY_HIDDEN PTHFileData {
  const uint32_t TokenOff;
  const uint32_t PPCondOff;
public:
  PTHFileData(uint32_t tokenOff, uint32_t ppCondOff)
    : TokenOff(tokenOff), PPCondOff(ppCondOff) {}
    
  uint32_t getTokenOffset() const { return TokenOff; }  
  uint32_t getPPCondOffset() const { return PPCondOff; }  
};
  
  
class VISIBILITY_HIDDEN PTHFileLookupCommonTrait {
public:
  typedef std::pair<unsigned char, const char*> internal_key_type;

  static unsigned ComputeHash(internal_key_type x) {
    return BernsteinHash(x.second);
  }
  
  static std::pair<unsigned, unsigned>
  ReadKeyDataLength(const unsigned char*& d) {
    unsigned keyLen = (unsigned) ReadUnalignedLE16(d);
    unsigned dataLen = (unsigned) *(d++);
    return std::make_pair(keyLen, dataLen);
  }
  
  static internal_key_type ReadKey(const unsigned char* d, unsigned) {
    unsigned char k = *(d++); // Read the entry kind.
    return std::make_pair(k, (const char*) d);
  }
};
  
class VISIBILITY_HIDDEN PTHFileLookupTrait : public PTHFileLookupCommonTrait {
public:
  typedef const FileEntry* external_key_type;
  typedef PTHFileData      data_type;
  
  static internal_key_type GetInternalKey(const FileEntry* FE) {
    return std::make_pair((unsigned char) 0x1, FE->getName());
  }

  static bool EqualKey(internal_key_type a, internal_key_type b) {
    return a.first == b.first && strcmp(a.second, b.second) == 0;
  }  
  
  static PTHFileData ReadData(const internal_key_type& k, 
                              const unsigned char* d, unsigned) {    
    assert(k.first == 0x1 && "Only file lookups can match!");
    uint32_t x = ::ReadUnalignedLE32(d);
    uint32_t y = ::ReadUnalignedLE32(d);
    return PTHFileData(x, y); 
  }
};

class VISIBILITY_HIDDEN PTHStringLookupTrait {
public:
  typedef uint32_t 
          data_type;

  typedef const std::pair<const char*, unsigned>
          external_key_type;

  typedef external_key_type internal_key_type;
  
  static bool EqualKey(const internal_key_type& a,
                       const internal_key_type& b) {
    return (a.second == b.second) ? memcmp(a.first, b.first, a.second) == 0
                                  : false;
  }
  
  static unsigned ComputeHash(const internal_key_type& a) {
    return BernsteinHash(a.first, a.second);
  }
  
  // This hopefully will just get inlined and removed by the optimizer.
  static const internal_key_type&
  GetInternalKey(const external_key_type& x) { return x; }
  
  static std::pair<unsigned, unsigned>
  ReadKeyDataLength(const unsigned char*& d) {
    return std::make_pair((unsigned) ReadUnalignedLE16(d), sizeof(uint32_t));
  }
    
  static std::pair<const char*, unsigned>
  ReadKey(const unsigned char* d, unsigned n) {
      assert(n >= 2 && d[n-1] == '\0');
      return std::make_pair((const char*) d, n-1);
    }
    
  static uint32_t ReadData(const internal_key_type& k, const unsigned char* d,
                           unsigned) {
    return ::ReadUnalignedLE32(d);
  }
};
  
} // end anonymous namespace  

typedef OnDiskChainedHashTable<PTHFileLookupTrait>   PTHFileLookup;
typedef OnDiskChainedHashTable<PTHStringLookupTrait> PTHStringIdLookup;

//===----------------------------------------------------------------------===//
// PTHManager methods.
//===----------------------------------------------------------------------===//

PTHManager::PTHManager(const llvm::MemoryBuffer* buf, void* fileLookup,
                       const unsigned char* idDataTable,
                       IdentifierInfo** perIDCache, 
                       void* stringIdLookup, unsigned numIds,
                       const unsigned char* spellingBase,
                       const char* originalSourceFile)
: Buf(buf), PerIDCache(perIDCache), FileLookup(fileLookup),
  IdDataTable(idDataTable), StringIdLookup(stringIdLookup),
  NumIds(numIds), PP(0), SpellingBase(spellingBase),
  OriginalSourceFile(originalSourceFile) {}

PTHManager::~PTHManager() {
  delete Buf;
  delete (PTHFileLookup*) FileLookup;
  delete (PTHStringIdLookup*) StringIdLookup;
  free(PerIDCache);
}

static void InvalidPTH(Diagnostic *Diags, Diagnostic::Level level,
                       const char* Msg = 0) {
  if (!Diags) return;  
  if (!Msg) Msg = "Invalid or corrupted PTH file";
  unsigned DiagID = Diags->getCustomDiagID(level, Msg);
  Diags->Report(FullSourceLoc(), DiagID);
}

PTHManager* PTHManager::Create(const std::string& file, Diagnostic* Diags,
                               Diagnostic::Level level) {
  // Memory map the PTH file.
  llvm::OwningPtr<llvm::MemoryBuffer>
  File(llvm::MemoryBuffer::getFile(file.c_str()));
  
  if (!File) {
    if (Diags) {
      unsigned DiagID = Diags->getCustomDiagID(level,
                                               "PTH file %0 could not be read");
      Diags->Report(FullSourceLoc(), DiagID) << file;
    }

    return 0;
  }
  
  // Get the buffer ranges and check if there are at least three 32-bit
  // words at the end of the file.
  const unsigned char* BufBeg = (unsigned char*)File->getBufferStart();
  const unsigned char* BufEnd = (unsigned char*)File->getBufferEnd();

  // Check the prologue of the file.
  if ((BufEnd - BufBeg) < (signed) (sizeof("cfe-pth") + 3 + 4) ||
      memcmp(BufBeg, "cfe-pth", sizeof("cfe-pth") - 1) != 0) {
    InvalidPTH(Diags, level);
    return 0;
  }
  
  // Read the PTH version.
  const unsigned char *p = BufBeg + (sizeof("cfe-pth") - 1);
  unsigned Version = ReadLE32(p);
  
  if (Version != PTHManager::Version) {
    InvalidPTH(Diags, level,
        Version < PTHManager::Version 
        ? "PTH file uses an older PTH format that is no longer supported"
        : "PTH file uses a newer PTH format that cannot be read");
    return 0;
  }

  // Compute the address of the index table at the end of the PTH file.  
  const unsigned char *PrologueOffset = p;
  
  if (PrologueOffset >= BufEnd) {
    InvalidPTH(Diags, level);
    return 0;
  }
  
  // Construct the file lookup table.  This will be used for mapping from
  // FileEntry*'s to cached tokens.
  const unsigned char* FileTableOffset = PrologueOffset + sizeof(uint32_t)*2;
  const unsigned char* FileTable = BufBeg + ReadLE32(FileTableOffset);
  
  if (!(FileTable > BufBeg && FileTable < BufEnd)) {
    InvalidPTH(Diags, level);
    return 0; // FIXME: Proper error diagnostic?
  }
  
  llvm::OwningPtr<PTHFileLookup> FL(PTHFileLookup::Create(FileTable, BufBeg));
  
  // Warn if the PTH file is empty.  We still want to create a PTHManager
  // as the PTH could be used with -include-pth.
  if (FL->isEmpty())
    InvalidPTH(Diags, level, "PTH file contains no cached source data");
  
  // Get the location of the table mapping from persistent ids to the
  // data needed to reconstruct identifiers.
  const unsigned char* IDTableOffset = PrologueOffset + sizeof(uint32_t)*0;
  const unsigned char* IData = BufBeg + ReadLE32(IDTableOffset);
  
  if (!(IData >= BufBeg && IData < BufEnd)) {
    InvalidPTH(Diags, level);
    return 0;
  }
  
  // Get the location of the hashtable mapping between strings and
  // persistent IDs.
  const unsigned char* StringIdTableOffset = PrologueOffset + sizeof(uint32_t)*1;
  const unsigned char* StringIdTable = BufBeg + ReadLE32(StringIdTableOffset);
  if (!(StringIdTable >= BufBeg && StringIdTable < BufEnd)) {
    InvalidPTH(Diags, level);
    return 0;
  }

  llvm::OwningPtr<PTHStringIdLookup> SL(PTHStringIdLookup::Create(StringIdTable,
                                                                  BufBeg));
  
  // Get the location of the spelling cache.
  const unsigned char* spellingBaseOffset = PrologueOffset + sizeof(uint32_t)*3;
  const unsigned char* spellingBase = BufBeg + ReadLE32(spellingBaseOffset);
  if (!(spellingBase >= BufBeg && spellingBase < BufEnd)) {
    InvalidPTH(Diags, level);
    return 0;
  }
  
  // Get the number of IdentifierInfos and pre-allocate the identifier cache.
  uint32_t NumIds = ReadLE32(IData);
  
  // Pre-allocate the peristent ID -> IdentifierInfo* cache.  We use calloc()
  // so that we in the best case only zero out memory once when the OS returns
  // us new pages.
  IdentifierInfo** PerIDCache = 0;
  
  if (NumIds) {
    PerIDCache = (IdentifierInfo**)calloc(NumIds, sizeof(*PerIDCache));  
    if (!PerIDCache) {
      InvalidPTH(Diags, level, 
                 "Could not allocate memory for processing PTH file");
      return 0;
    }
  }

  // Compute the address of the original source file.
  const unsigned char* originalSourceBase = PrologueOffset + sizeof(uint32_t)*4;
  unsigned len = ReadUnalignedLE16(originalSourceBase);
  if (!len) originalSourceBase = 0;  
  
  // Create the new PTHManager.
  return new PTHManager(File.take(), FL.take(), IData, PerIDCache,
                        SL.take(), NumIds, spellingBase,
                        (const char*) originalSourceBase);
}

IdentifierInfo* PTHManager::LazilyCreateIdentifierInfo(unsigned PersistentID) {
  // Look in the PTH file for the string data for the IdentifierInfo object.
  const unsigned char* TableEntry = IdDataTable + sizeof(uint32_t)*PersistentID;
  const unsigned char* IDData =
    (const unsigned char*)Buf->getBufferStart() + ReadLE32(TableEntry);
  assert(IDData < (const unsigned char*)Buf->getBufferEnd());
  
  // Allocate the object.
  std::pair<IdentifierInfo,const unsigned char*> *Mem =
    Alloc.Allocate<std::pair<IdentifierInfo,const unsigned char*> >();

  Mem->second = IDData;
  assert(IDData[0] != '\0');
  IdentifierInfo *II = new ((void*) Mem) IdentifierInfo();
  
  // Store the new IdentifierInfo in the cache.
  PerIDCache[PersistentID] = II;
  assert(II->getName() && II->getName()[0] != '\0');
  return II;
}

IdentifierInfo* PTHManager::get(const char *NameStart, const char *NameEnd) {
  PTHStringIdLookup& SL = *((PTHStringIdLookup*)StringIdLookup);
  // Double check our assumption that the last character isn't '\0'.
  assert(NameEnd==NameStart || NameStart[NameEnd-NameStart-1] != '\0');
  PTHStringIdLookup::iterator I = SL.find(std::make_pair(NameStart,
                                                         NameEnd - NameStart));
  if (I == SL.end()) // No identifier found?
    return 0;

  // Match found.  Return the identifier!
  assert(*I > 0);
  return GetIdentifierInfo(*I-1);
}

PTHLexer *PTHManager::CreateLexer(FileID FID) {
  const FileEntry *FE = PP->getSourceManager().getFileEntryForID(FID);
  if (!FE)
    return 0;
  
  // Lookup the FileEntry object in our file lookup data structure.  It will
  // return a variant that indicates whether or not there is an offset within
  // the PTH file that contains cached tokens.
  PTHFileLookup& PFL = *((PTHFileLookup*)FileLookup);
  PTHFileLookup::iterator I = PFL.find(FE);
  
  if (I == PFL.end()) // No tokens available?
    return 0;
  
  const PTHFileData& FileData = *I;  
  
  const unsigned char *BufStart = (const unsigned char *)Buf->getBufferStart();
  // Compute the offset of the token data within the buffer.
  const unsigned char* data = BufStart + FileData.getTokenOffset();

  // Get the location of pp-conditional table.
  const unsigned char* ppcond = BufStart + FileData.getPPCondOffset();
  uint32_t Len = ReadLE32(ppcond);
  if (Len == 0) ppcond = 0;
  
  assert(PP && "No preprocessor set yet!");
  return new PTHLexer(*PP, FID, data, ppcond, *this); 
}

//===----------------------------------------------------------------------===//
// 'stat' caching.
//===----------------------------------------------------------------------===//

namespace {
class VISIBILITY_HIDDEN PTHStatData {
public:
  const bool hasStat;
  const ino_t ino;
  const dev_t dev;
  const mode_t mode;
  const time_t mtime;
  const off_t size;
  
  PTHStatData(ino_t i, dev_t d, mode_t mo, time_t m, off_t s)
  : hasStat(true), ino(i), dev(d), mode(mo), mtime(m), size(s) {}  
  
  PTHStatData()
    : hasStat(false), ino(0), dev(0), mode(0), mtime(0), size(0) {}
};
  
class VISIBILITY_HIDDEN PTHStatLookupTrait : public PTHFileLookupCommonTrait {
public:
  typedef const char* external_key_type;  // const char*
  typedef PTHStatData data_type;
    
  static internal_key_type GetInternalKey(const char *path) {
    // The key 'kind' doesn't matter here because it is ignored in EqualKey.
    return std::make_pair((unsigned char) 0x0, path);
  }

  static bool EqualKey(internal_key_type a, internal_key_type b) {
    // When doing 'stat' lookups we don't care about the kind of 'a' and 'b',
    // just the paths.
    return strcmp(a.second, b.second) == 0;
  }  
  
  static data_type ReadData(const internal_key_type& k, const unsigned char* d,
                            unsigned) {    
    
    if (k.first /* File or Directory */) {
      if (k.first == 0x1 /* File */) d += 4 * 2; // Skip the first 2 words.
      ino_t ino = (ino_t) ReadUnalignedLE32(d);
      dev_t dev = (dev_t) ReadUnalignedLE32(d);
      mode_t mode = (mode_t) ReadUnalignedLE16(d);
      time_t mtime = (time_t) ReadUnalignedLE64(d);    
      return data_type(ino, dev, mode, mtime, (off_t) ReadUnalignedLE64(d));
    }

    // Negative stat.  Don't read anything.
    return data_type();
  }
};

class VISIBILITY_HIDDEN PTHStatCache : public StatSysCallCache {
  typedef OnDiskChainedHashTable<PTHStatLookupTrait> CacheTy;
  CacheTy Cache;

public:  
  PTHStatCache(PTHFileLookup &FL) :
    Cache(FL.getNumBuckets(), FL.getNumEntries(), FL.getBuckets(),
          FL.getBase()) {}

  ~PTHStatCache() {}
  
  int stat(const char *path, struct stat *buf) {
    // Do the lookup for the file's data in the PTH file.
    CacheTy::iterator I = Cache.find(path);

    // If we don't get a hit in the PTH file just forward to 'stat'.
    if (I == Cache.end()) return ::stat(path, buf);
    
    const PTHStatData& Data = *I;
    
    if (!Data.hasStat)
      return 1;

    buf->st_ino = Data.ino;
    buf->st_dev = Data.dev;
    buf->st_mtime = Data.mtime;
    buf->st_mode = Data.mode;
    buf->st_size = Data.size;
    return 0;
  }
};
} // end anonymous namespace

StatSysCallCache *PTHManager::createStatCache() {
  return new PTHStatCache(*((PTHFileLookup*) FileLookup));
}
