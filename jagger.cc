// Jagger -- C++ implementation of Pattern-based Japanese Morphological Analyzer
//  $Id: jagger.cc 2070 2024-03-14 07:54:57Z ynaga $
// Copyright (c) 2022 Naoki Yoshinaga <ynaga@iis.u-tokyo.ac.jp>
#include <jagger.h>
#include <iostream>

#ifndef _WIN32
#define _isatty ::isatty
#define _mmap ::mmap
#define _munmap ::munmap
#define __open ::open
#endif

#ifdef _WIN32
#include "getopt.h"
#define PROT_READ    0x1  // Pages can be read
#define PROT_WRITE   0x2  // Pages can be written to
#define PROT_EXEC    0x4  // Pages can be executed
#define PROT_NONE    0x0  // Pages cannot be accessed
#define PAGE_READONLY    0x02
#define PAGE_READWRITE   0x04
#define PAGE_EXECUTE     0x10
#define PAGE_NOACCESS    0x01
#define MAP_SHARED (FILE_MAP_READ | FILE_MAP_WRITE)
#define JAGGER_DEFAULT_MODEL "..\\model\\kyoto+kwdlc"

void* _mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    HANDLE hFile = INVALID_HANDLE_VALUE;
    if (fd != -1) {
        hFile = (HANDLE)_get_osfhandle(fd);  // Convert file descriptor to HANDLE
        if (hFile == INVALID_HANDLE_VALUE) return NULL;  // Invalid file handle
    }

    // Open or create file mapping
    HANDLE hMap = CreateFileMapping(hFile, 
        NULL, 
        PAGE_READWRITE, 
        0, 
        0, 
        NULL);

    if (hMap == NULL) {
        DWORD lastError = GetLastError();
        return NULL;
    }

    // Create a view of the file (map memory)
    void* mappedAddr = MapViewOfFile(hMap, 
        FILE_MAP_WRITE, 
        0, 
        0, 
        length);

    if (mappedAddr == NULL) {
        DWORD lastError = GetLastError();
        CloseHandle(hMap);
        return NULL;
    }

    // Return mapped memory address
    return mappedAddr;
}

void _munmap(void* addr, size_t length) {
    UnmapViewOfFile(addr);
}

void utf8_to_wide(const char* utf8_str, std::wstring& utf16_str) {

    if (utf8_str == NULL) {
        return;
    }

    int len = MultiByteToWideChar(CP_UTF8, 0, utf8_str, -1, NULL, 0);
    if (len == 0) {
        return;
    }

    std::vector<unsigned char>buf((len + 1) * sizeof(wchar_t));
    if (MultiByteToWideChar(CP_UTF8,
        0, utf8_str,
        -1,
        (LPWSTR)&buf[0],
        len)) {
        utf16_str = std::wstring((const wchar_t*)&buf[0]);
    }

    return;
}

void wide_to_utf8(const wchar_t* utf16_str, std::string& utf8_str) {

    if (utf16_str == NULL) {
        return;
    }

    int len = WideCharToMultiByte(CP_UTF8, 0, utf16_str, -1, NULL, 0, NULL, NULL);
    if (len == 0) {
        return;
    }

    std::vector<unsigned char>buf((len + 1) * sizeof(char));
    if (WideCharToMultiByte(CP_UTF8,
        0, utf16_str,
        -1,
        (LPSTR)&buf[0],
        len, NULL, NULL)) {
        utf8_str = std::string((const char*)&buf[0]);
    }

    return;
}

void expand_path(std::string& m, char* currrent_directory_path) {
    std::wstring w;
    utf8_to_wide(m.c_str(), w);
    std::wstring absolute_path;
    utf8_to_wide(currrent_directory_path, absolute_path);
    std::vector<wchar_t>buf(MAX_PATH + 1);
    memcpy(&buf[0], &absolute_path[0], absolute_path.length() * sizeof(wchar_t));

    if (PathRemoveFileSpecW(&buf[0])) {
        absolute_path = &buf[0];
        SetCurrentDirectoryW(absolute_path.c_str());
        GetFullPathNameW(w.c_str(), MAX_PATH, &buf[0], NULL);
        wide_to_utf8(&buf[0], m);
    }
}

int __open(const char* utf8_path, int oflag, ...) {

    std::wstring wide_path;

    utf8_to_wide(utf8_path, wide_path);
    if (wide_path.length() == 0) {
        return -1;  // Conversion failed
    }

    // Use CreateFileW to open the file with the appropriate flags
    HANDLE hFile = CreateFileW(
        wide_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD lastError = GetLastError();
        return -1;  // Return error if CreateFileW fails
    }

    // Convert the file handle to a file descriptor (for compatibility with _open)
    return _open_osfhandle((intptr_t)hFile, 0);  // The second argument 0 indicates no extra flags
}

off_t __lseek(int fd, off_t offset, int whence) {
    // Convert the file descriptor to a Windows HANDLE
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);

    if (hFile == INVALID_HANDLE_VALUE) {
        return -1;  // Invalid file descriptor
    }

    DWORD dwMoveMethod = 0;
    switch (whence) {
    case SEEK_SET:
        dwMoveMethod = FILE_BEGIN;
        break;
    case SEEK_CUR:
        dwMoveMethod = FILE_CURRENT;
        break;
    case SEEK_END:
        dwMoveMethod = FILE_END;
        break;
    default:
        return -1;  // Invalid whence value
    }

    // Move the file pointer to the specified offset
    LARGE_INTEGER liOffset;
    liOffset.QuadPart = offset;

    // Use SetFilePointerEx to set the file pointer
    LARGE_INTEGER liNewPointer;
    if (SetFilePointerEx(hFile, liOffset, &liNewPointer, dwMoveMethod)) {
        return (off_t)liNewPointer.QuadPart;
    }

    // If SetFilePointerEx fails, return -1
    return -1;
}
#endif

namespace jagger {
  class tagger {
  private:
    ccedar::da_  _da;  // there may be cache friendly alignment
    uint16_t*    _c2i; // UTF8 char and BOS -> id
    feat_info_t* _p2f; // pattern id -> feature (info)
    char*        _fs;  // feature strings
    std::vector <std::pair <void*, size_t> > _mmaped;
    void* _read_array (const std::string& fn) {
      int fd = __open(fn.c_str (), O_RDONLY);
      ERR_IF (fd == -1, "no such file: %s", fn.c_str ());
      const size_t size = __lseek(fd, 0, SEEK_END); // get size
      __lseek(fd, 0, SEEK_SET);
      void *data = _mmap (0, size, PROT_READ, MAP_SHARED, fd, 0);
      _close (fd);
      _mmaped.push_back (std::make_pair (data, size));
      return data;
    }
  public:
    tagger () : _da (), _c2i (0), _p2f (0), _fs (0), _mmaped () {}
    ~tagger () {
      for (size_t i = 0; i < _mmaped.size (); ++i)
        _munmap (_mmaped[i].first, _mmaped[i].second);
    }
    void read_model (const std::string& m) { // read patterns
      _da.set_array (_read_array (m + ".da"));
      _c2i = static_cast <uint16_t*> (_read_array (m + ".c2i"));
      _p2f = static_cast <feat_info_t*> (_read_array (m + ".p2f"));
      _fs  = static_cast <char*> (_read_array (m + ".fs"));
    }
    void write_feature (simple_writer& writer, const bool concat, const feat_info_t finfo) const {
      IF_COMPACT (writer.write (&_fs[finfo.core_feat_offset], finfo.core_feat_len));
      if (concat) { // as unknown words
        IF_NOT_COMPACT (writer.write (&_fs[finfo.feat_offset], finfo.core_feat_len));
        writer.write (",*,*,*\n", 7);
      } else
        writer.write (&_fs[finfo.feat_offset], finfo.feat_len);
    }
    template <const bool TAGGING, const bool TTY>
    void run () const {
      union { struct { uint32_t shift : MAX_PATTERN_BITS, ctype : 4, id : 20; bool concat : 1; }; int r; } s_prev = {}, s = {};
      feat_info_t finfo = { _c2i[CP_MAX + 1] }; // BOS
      simple_reader reader;
      simple_writer writer;
      for (;! reader.eob ();) {
        if (*reader.ptr () == '\n') { // EOS
          if (s_prev.r)
            if (TAGGING) write_feature (writer, s_prev.concat, finfo);
          writer.write (TAGGING ? "EOS\n" : "\n", TAGGING ? 4 : 1);
          s.shift = 1;
          s_prev.r = 0; // *
          finfo.ti = _c2i[CP_MAX + 1]; // BOS
          if (TTY) writer.flush (); // line buffering
        } else {
          s.r = _da.longestPatternSearch (reader.ptr (), reader.end (), finfo.ti, _c2i);
          if (! s.shift) s.shift = u8_len (reader.ptr ());
          if (s_prev.r &&  // word that may concat with the future context
              ! (s.concat = (s_prev.ctype == s.ctype && // char type mismatch
                             s_prev.ctype != OTHER &&   // kanji, symbol
                             (s_prev.ctype != KANA || s_prev.shift + s.shift < 18)))) {
            if (TAGGING)
              write_feature (writer, s_prev.concat, finfo);
            else
              writer.write (" ", 1);
          }
          finfo = _p2f[s.id];
          s_prev = s; // *
          writer.write (reader.ptr (), s.shift);
        }
        reader.advance (s.shift);
        if (! TTY && ! writer.writable (1 << MAX_FEATURE_BITS)) writer.flush ();
        if (TTY && reader.eob ()) reader.read ();
        if (! TTY && ! reader.readable (1 << MAX_PATTERN_BITS)) reader.read ();
      }
      if (s_prev.r) {
        if (TAGGING) write_feature (writer, s_prev.concat, finfo);
        writer.write (TAGGING ? "EOS\n" : "\n", TAGGING ? 4 : 1);
      }
    }
  };
}

int main (int argc, char** argv) {
    
    std::string m (JAGGER_DEFAULT_MODEL "/patterns");
    
#ifdef _WIN32
    expand_path(m, argv[0]);
#endif
    
  bool tagging = true;
  bool interactive = false;
  { // options (minimal)
    for (int opt = 0; (opt = getopt(argc, argv, "m:u:whc")) != -1;)
      switch (opt) {
        case 'm': 
        {
            m = optarg; 
            if (!m.empty()) {
                char lastChar = m.back();
                if (lastChar == '\\') {
                    m.pop_back();
                }
            }
            m += "\\patterns"; 
        break;
        }
        case 'c': interactive = true; break;
        case 'w': tagging = false; break;
        case 'h': errx (1, "Pattern-based Jappanese Morphological Analyzer\nCopyright (c) 2023- Naoki Yoshinaga, All rights reserved.\n\nUsage: %s [-m dir w] < input\n\nOptions:\n -m dir\tdirectory for compiled patterns (default: " JAGGER_DEFAULT_MODEL ")\n -w\tperform only segmentation\n", argv[0]);
      }
  }

  jagger::tagger jagger;
  jagger.read_model(m);

  if ((_isatty(0) == 1)||(interactive)){ // interactive IO
          if (tagging) jagger.run <true, true>(); else jagger.run <false, true>();
      }
  else { // batch
      if (tagging) jagger.run <true, false>(); else jagger.run <false, false>();
  }

  return 0;
}
