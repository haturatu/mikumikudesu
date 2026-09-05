#pragma once

#include <filesystem>
#include <istream>
#include <streambuf>
#include <vector>

namespace dayo::core {

class MappedFileStream final : public std::istream {
  public:
    explicit MappedFileStream(const std::filesystem::path& path);
    ~MappedFileStream() override;
    MappedFileStream(const MappedFileStream&) = delete;
    MappedFileStream& operator=(const MappedFileStream&) = delete;

  private:
    class Buffer final : public std::streambuf {
      public:
        explicit Buffer(const std::filesystem::path& path);
        ~Buffer() override;
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;

      private:
        [[nodiscard]] std::size_t size() const noexcept;
        pos_type seekoff(off_type off, std::ios_base::seekdir way, std::ios_base::openmode which) override;
        pos_type seekpos(pos_type position, std::ios_base::openmode which) override;
        void close() noexcept;

        const char* mapped_{nullptr};
        std::size_t mappedSize_{};
        int fileDescriptor_{-1};
        std::vector<char> fallback_;
    } buffer_;
};

} // namespace dayo::core
