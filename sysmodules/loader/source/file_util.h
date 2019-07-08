#pragma once

#include <optional>
#include <string.h>
#include <type_traits>

extern "C" {
#include <3ds/result.h>
#include <3ds/services/fs.h>
#include <3ds/svc.h>
#include <3ds/types.h>
}

namespace util {

inline FS_Path MakePath(const char* path) {
    return {PATH_ASCII, strnlen(path, 255) + 1, path};
}

// A small wrapper to make forgetting to close a file and
// to check read lengths impossible.
class File {
public:
    File() = default;
    File(const File& other) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) { *this = std::move(other); }
    File& operator=(File&& other) {
        std::swap(m_handle, other.m_handle);
        return *this;
    }

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

    bool Read(void* buffer, u32 size, u64 offset) {
        u32 bytes_read = 0;
        const Result res = FSFILE_Read(*m_handle, &bytes_read, offset, buffer, size);
        return R_SUCCEEDED(res) && bytes_read == size;
    }

    std::optional<u64> GetSize() const {
        u64 size;
        if (!R_SUCCEEDED(FSFILE_GetSize(*m_handle, &size)))
            return std::nullopt;
        return size;
    }

private:
    std::optional<Handle> m_handle;
};

// A small utility class that provides file-like reading for an in-memory buffer.
class MemoryStream {
public:
    MemoryStream(u8* ptr, u32 size) : m_ptr{ptr}, m_size{size} {}

    void Read(void* buffer, u32 read_length) {
        memcpy(buffer, m_ptr + m_offset, read_length);
        m_offset += read_length;
    }

    template <typename T>
    T Read() {
        static_assert(std::is_pod_v<T>);
        T val{};
        Read(&val, sizeof(val));
        return val;
    }

    u8* data() const { return m_ptr; }
    u32 size() const { return m_size; }
    u8* begin() const { return m_ptr; }
    u8* end() const { return m_ptr + m_size; }
    u8& operator[](size_t pos) const { return m_ptr[pos]; }

    u32 Tell() const { return m_offset; }
    void Seek(u32 offset) { m_offset = offset; }

private:
    u8* m_ptr = nullptr;
    u32 m_size = 0;
    u32 m_offset = 0;
};

}  // namespace util
