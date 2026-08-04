#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <propidl.h>

#include <cstdint>

namespace modlist::sevenzip_sdk {

using UInt32 = std::uint32_t;
using Int32 = std::int32_t;
using UInt64 = std::uint64_t;
using Int64 = std::int64_t;

inline constexpr GUID kIidSequentialInStream =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00}};
inline constexpr GUID kIidSequentialOutStream =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00}};
inline constexpr GUID kIidInStream =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00}};
inline constexpr GUID kIidProgress =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00}};
inline constexpr GUID kIidArchiveOpenCallback =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00}};
inline constexpr GUID kIidArchiveExtractCallback =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x20, 0x00, 0x00}};
inline constexpr GUID kIidInArchive =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x60, 0x00, 0x00}};
inline constexpr GUID kIidSetProperties =
    {0x23170F69, 0x40C1, 0x278A, {0x00, 0x00, 0x00, 0x06, 0x00, 0x03, 0x00, 0x00}};
inline constexpr GUID kClsid7zFormat =
    {0x23170F69, 0x40C1, 0x278A, {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}};

struct __declspec(novtable) ISequentialInStream : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processedSize) = 0;
};

struct __declspec(novtable) ISequentialOutStream : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* processedSize) = 0;
};

struct __declspec(novtable) IInStream : ISequentialInStream {
  virtual HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 seekOrigin, UInt64* newPosition) = 0;
};

struct __declspec(novtable) IProgress : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* completeValue) = 0;
};

struct __declspec(novtable) IArchiveOpenCallback : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetTotal(const UInt64* files, const UInt64* bytes) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* files, const UInt64* bytes) = 0;
};

struct __declspec(novtable) IArchiveExtractCallback : IProgress {
  virtual HRESULT STDMETHODCALLTYPE GetStream(
      UInt32 index, ISequentialOutStream** outStream, Int32 askExtractMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE PrepareOperation(Int32 askExtractMode) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 operationResult) = 0;
};

struct __declspec(novtable) ISetProperties : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetProperties(
      const wchar_t* const* names, const PROPVARIANT* values, UInt32 numProps) = 0;
};

struct __declspec(novtable) IInArchive : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Open(
      IInStream* stream, const UInt64* maxCheckStartPosition,
      IArchiveOpenCallback* openCallback) = 0;
  virtual HRESULT STDMETHODCALLTYPE Close() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfItems(UInt32* numItems) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProperty(UInt32 index, PROPID propID, PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE Extract(
      const UInt32* indices, UInt32 numItems, Int32 testMode,
      IArchiveExtractCallback* extractCallback) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetArchiveProperty(PROPID propID, PROPVARIANT* value) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfProperties(UInt32* numProps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyInfo(
      UInt32 index, BSTR* name, PROPID* propID, VARTYPE* varType) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetNumberOfArchiveProperties(UInt32* numProps) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetArchivePropertyInfo(
      UInt32 index, BSTR* name, PROPID* propID, VARTYPE* varType) = 0;
};

using CreateObjectFn = HRESULT(WINAPI*)(const GUID* classId, const GUID* interfaceId, void** object);

inline constexpr PROPID kPropPath = 3;
inline constexpr PROPID kPropIsDir = 6;
inline constexpr PROPID kPropAttrib = 9;
inline constexpr PROPID kPropMTime = 12;

inline constexpr Int32 kAskExtract = 0;
inline constexpr Int32 kOperationOk = 0;

}  // namespace modlist::sevenzip_sdk
