#include "bps_patcher.h"

#include <array>
#include <string_view>

extern "C" {
#include <3ds/os.h>
#include <3ds/result.h>
#include <3ds/services/fs.h>
#include <3ds/svc.h>

#include "patcher.h"
#include "strings.h"
}

#include "Crc32.h"
#include "file_util.h"

namespace patcher {

class ScopedAppHeap {
public:
  ScopedAppHeap() {
    u32 tmp;
    m_size = osGetMemRegionFree(MEMREGION_APPLICATION);
    if (!R_SUCCEEDED(svcControlMemory(&tmp, BaseAddress, 0, m_size,
                                      MemOp(MEMOP_ALLOC | MEMOP_REGION_APP),
                                      MemPerm(MEMPERM_READ | MEMPERM_WRITE)))) {
      svcBreak(USERBREAK_PANIC);
    }
  }

  ~ScopedAppHeap() {
    u32 tmp;
    svcControlMemory(&tmp, BaseAddress, 0, m_size, MEMOP_FREE, MemPerm(0));
  }

  static constexpr u32 BaseAddress = 0x08000000;

private:
  u32 m_size;
};

namespace bps {

using Number = u32;

Number Decode(util::MemoryStream& stream) {
  Number data = 0, shift = 1;
  while (true) {
    const u8 x = stream.Read<u8>();
    data += (x & 0x7f) * shift;
    if (x & 0x80)
      break;
    shift <<= 7;
    data += shift;
  }
  return data;
}

class PatchApplier {
public:
  // patch should point at the start of the commands.
  PatchApplier(util::MemoryStream source, util::MemoryStream target, util::MemoryStream patch)
      : m_source{source}, m_target{target}, m_patch{patch} {}

  bool Apply() {
    const u32 command_start_offset = m_patch.Tell();
    const u32 command_end_offset = m_patch.size() - 12;
    m_patch.Seek(command_end_offset);
    const u32 source_crc32 = m_patch.Read<u32>();
    const u32 target_crc32 = m_patch.Read<u32>();
    m_patch.Seek(command_start_offset);

    // Ensure we are patching the right executable.
    if (crc32_fast(m_source.data(), m_source.size()) != source_crc32) {
      svcBreak(USERBREAK_USER);
      return false;
    }

    // Process all patch commands.
    while (m_patch.Tell() < command_end_offset) {
      const bool ok = HandleCommand();
      if (!ok) {
        svcBreak(USERBREAK_PANIC);
        return false;
      }
    }

    // Verify that the executable was patched correctly.
    if (crc32_fast(m_target.data(), m_target.size()) != target_crc32) {
      svcBreak(USERBREAK_PANIC);
      return false;
    }

    return true;
  }

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
    m_source.Read(m_target.data() + m_output_offset, length, m_output_offset);
    m_output_offset += length;
    return true;
  }

  bool TargetRead(Number length) {
    m_patch.Read(m_target.data() + m_output_offset, length);
    m_output_offset += length;
    return true;
  }

  bool SourceCopy(Number length) {
    const Number data = Decode(m_patch);
    m_source_relative_offset += (data & 1 ? -1 : +1) * (data >> 1);
    m_source.Read(m_target.data() + m_output_offset, length, m_source_relative_offset);
    m_output_offset += length;
    m_source_relative_offset += length;
    return true;
  }

  bool TargetCopy(Number length) {
    const Number data = Decode(m_patch);
    m_target_relative_offset += (data & 1 ? -1 : +1) * (data >> 1);
    while (length--)
      m_target[m_output_offset++] = m_target[m_target_relative_offset++];
    return true;
  }

  u32 m_source_relative_offset = 0;
  u32 m_target_relative_offset = 0;
  u32 m_output_offset = 0;
  util::MemoryStream m_source, m_target, m_patch;
};

}  // namespace bps

inline bool ApplyCodeBpsPatch(u64 progId, u8* code, u32 size) {
  char bps_path[] = "/luma/titles/0000000000000000/code.bps";
  progIdToStr(bps_path + 28, progId);
  util::File patch_file;
  if (!patch_file.Open(bps_path, FS_OPEN_READ))
    return true;

  ScopedAppHeap memory;

  const u64 patch_size = patch_file.GetSize().value_or(0);
  util::MemoryStream patch{(u8*)memory.BaseAddress, static_cast<u32>(patch_size)};
  if (!patch_file.Read(patch.data(), patch.size(), 0)) {
    svcBreak(USERBREAK_PANIC);
    return false;
  }

  const auto magic = patch.Read<std::array<char, 4>>();
  if (std::string_view(magic.data(), magic.size()) != "BPS1") {
    svcBreak(USERBREAK_ASSERT);
    return false;
  }

  const bps::Number source_size = bps::Decode(patch);
  const bps::Number target_size = bps::Decode(patch);
  const bps::Number metadata_size = bps::Decode(patch);
  if (std::max(source_size, target_size) > size || metadata_size != 0) {
    svcBreak(USERBREAK_ASSERT);
    return false;
  }

  util::MemoryStream source{patch.end(), source_size};
  // Patch in-place. source and patch will be deallocated when patching is complete.
  memcpy(source.data(), code, source.size());
  memset(code, 0, size);
  util::MemoryStream target{code, target_size};

  bps::PatchApplier applier{source, target, patch};
  return applier.Apply();
}

}  // namespace patcher

extern "C" {
bool patcherApplyCodeBpsPatch(u64 progId, u8* code, u32 size) {
  return patcher::ApplyCodeBpsPatch(progId, code, size);
}
}
