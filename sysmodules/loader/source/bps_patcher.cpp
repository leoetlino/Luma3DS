#include "bps_patcher.h"

#include <optional>
#include <string.h>
#include <string_view>
#include <type_traits>

extern "C" {
#include <3ds/result.h>
#include <3ds/services/fs.h>

#include "patcher.h"
#include "strings.h"
}

namespace patcher {

constexpr const char TemporaryCodePath[] = "/luma/loader_tmp_code.bin";

namespace fs {

FS_Path MakePath(const char* path) {
  return {PATH_ASCII, strnlen(path, 255) + 1, path};
}

// A small wrapper to make forgetting to close a file impossible.
class File {
public:
  File() = default;
  File(const File& other) = delete;
  File& operator=(const File&) = delete;

  ~File() { Close(); }

  bool Close() {
    const bool ok = !m_handle || R_SUCCEEDED(FSFILE_Close(*m_handle));
    if (ok)
      m_handle = std::nullopt;
    return ok;
  }

  bool Open(const char* path, int open_flags) {
    const FS_Path archive_path = {PATH_EMPTY, 1, ""};
    Handle handle;
    const bool ok = R_SUCCEEDED(FSUSER_OpenFileDirectly(&handle, ARCHIVE_SDMC, archive_path,
                                                        MakePath(path), open_flags, 0));
    if (ok)
      m_handle = handle;
    return ok;
  }

  bool Read(void* buffer, u32 size) {
    u32 bytes_read = 0;
    const Result res = FSFILE_Read(*m_handle, &bytes_read, offset, buffer, size);
    const bool ok = R_SUCCEEDED(res) && bytes_read == size;
    if (ok)
      offset += size;
    return ok;
  }

  bool SeekAndRead(void* buffer, u32 size, u64 read_offset) {
    offset = read_offset;
    return Read(buffer, size);
  }

  template <typename T>
  bool Read(T& value) {
    static_assert(std::is_pod_v<T>);
    return Read(&value, sizeof(value));
  }

  bool Write(const void* buffer, u32 size) {
    u32 written = 0;
    const Result res = FSFILE_Write(*m_handle, &written, offset, buffer, size, 0);
    const bool ok = R_SUCCEEDED(res) && written == size;
    if (ok)
      offset += size;
    return ok;
  }

  std::optional<u64> GetSize() const {
    u64 size;
    if (!R_SUCCEEDED(FSFILE_GetSize(*m_handle, &size)))
      return std::nullopt;
    return size;
  }

  u64 offset = 0;

private:
  std::optional<Handle> m_handle;
};

void DeleteFile(const char* path) {
  FSUSER_DeleteFile(ARCHIVE_SDMC, MakePath(path));
}

}  // namespace fs

namespace bps {

using Number = u32;

Number Decode(fs::File& file) {
  Number data = 0, shift = 1;
  while (true) {
    u8 x;
    if (!file.Read(x))
      break;
    data += (x & 0x7f) * shift;
    if (x & 0x80)
      break;
    shift <<= 7;
    data += shift;
  }
  return data;
}

// A simple BPS patch applier.
class PatchApplier {
public:
  PatchApplier(u8* code, u32 size, fs::File& file) : m_target{code}, m_size{size}, m_patch{file} {}
  ~PatchApplier() {
    m_source.Close();
    fs::DeleteFile(TemporaryCodePath);
  }

  bool Apply();

private:
  bool HandleCommand() {
    const Number data = Decode(m_patch);
    const u32 command = data & 3;
    const u32 length = (data >> 2) + 1;
    switch (command) {
    case 0:
      return SourceRead(length);
    case 1:
      return TargetRead(length);
    case 2:
      return SourceCopy(length);
    case 3:
      return TargetCopy(length);
    default:
      return false;
    }
  }

  bool SourceRead(Number length) {
    const bool ok = m_source.SeekAndRead(m_target + m_output_offset, length, m_output_offset);
    m_output_offset += length;
    return ok;
  }

  bool TargetRead(Number length) {
    const bool ok = m_patch.Read(m_target + m_output_offset, length);
    m_output_offset += length;
    return ok;
  }

  bool SourceCopy(Number length) {
    const Number data = Decode(m_patch);
    m_source_relative_offset += (data & 1 ? -1 : +1) * (data >> 1);
    const bool ok = m_source.SeekAndRead(m_target + m_output_offset, length, m_source_relative_offset);
    m_output_offset += length;
    m_source_relative_offset += length;
    return ok;
  }

  bool TargetCopy(Number length) {
    const Number data = Decode(m_patch);
    m_target_relative_offset += (data & 1 ? -1 : +1) * (data >> 1);
    while (length--)
      m_target[m_output_offset++] = m_target[m_target_relative_offset++];
    return true;
  }

  bool Init() {
    if (!m_source.Open(TemporaryCodePath, FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE))
      return false;
    return m_source.Write(m_target, m_size);
  }

  u8* m_target = nullptr;
  u32 m_size = 0;
  fs::File& m_patch;
  fs::File m_source;
  u32 m_output_offset = 0;
  u32 m_source_relative_offset = 0;
  u32 m_target_relative_offset = 0;
};

bool PatchApplier::Apply() {
  if (!Init())
    return false;

  char magic[4];
  if (!m_patch.Read(magic) || std::string_view(magic, 4) != "BPS1")
    return false;

  const Number source_size = Decode(m_patch);
  (void)source_size;
  const Number target_size = Decode(m_patch);
  const Number metadata_size = Decode(m_patch);
  if (target_size > m_size)
    return false;
  if (metadata_size != 0)
    return false;

  const u64 patch_size = m_patch.GetSize().value_or(0);
  if (patch_size < 12)
    return false;

  while (m_patch.offset < patch_size - 12) {
    const bool ok = HandleCommand();
    if (!ok)
      return false;
  }

  // Ignore the checksums.

  return true;
}

}  // namespace bps

inline bool ApplyCodeBpsPatch(u64 progId, u8* code, u32 size) {
  char bps_path[] = "/luma/titles/0000000000000000/code.bps";
  progIdToStr(bps_path + 28, progId);
  fs::File bps_file;
  if (!bps_file.Open(bps_path, FS_OPEN_READ))
    return true;

  bps::PatchApplier applier{code, size, bps_file};
  return applier.Apply();
}

}  // namespace patcher

extern "C" {
bool patcherApplyCodeBpsPatch(u64 progId, u8* code, u32 size) {
  return patcher::ApplyCodeBpsPatch(progId, code, size);
}
}
