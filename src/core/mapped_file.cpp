#include "core/mapped_file.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dayo::core {

MappedFileStream::Buffer::Buffer(const std::filesystem::path& path) {
#if defined(__linux__)
    fileDescriptor_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fileDescriptor_ >= 0) {
        struct stat metadata {};
        if (::fstat(fileDescriptor_, &metadata) == 0 && metadata.st_size > 0) {
            mappedSize_ = static_cast<std::size_t>(metadata.st_size);
            void* address = ::mmap(nullptr, mappedSize_, PROT_READ, MAP_PRIVATE, fileDescriptor_, 0);
            if (address != MAP_FAILED) {
                mapped_ = static_cast<const char*>(address);
                setg(const_cast<char*>(mapped_), const_cast<char*>(mapped_),
                     const_cast<char*>(mapped_ + mappedSize_));
                return;
            }
        }
        close();
    }
#endif
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open file: " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0)
        throw std::runtime_error("cannot determine file size: " + path.string());
    fallback_.resize(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(fallback_.data(), static_cast<std::streamsize>(fallback_.size()));
    if (!input && !fallback_.empty())
        throw std::runtime_error("cannot read file: " + path.string());
    setg(fallback_.data(), fallback_.data(), fallback_.data() + fallback_.size());
}

MappedFileStream::Buffer::~Buffer() {
    close();
}

void MappedFileStream::Buffer::close() noexcept {
#if defined(__linux__)
    if (mapped_ != nullptr)
        ::munmap(const_cast<char*>(mapped_), mappedSize_);
    if (fileDescriptor_ >= 0)
        ::close(fileDescriptor_);
#endif
    mapped_ = nullptr;
    mappedSize_ = 0;
    fileDescriptor_ = -1;
}

MappedFileStream::MappedFileStream(const std::filesystem::path& path) : std::istream(nullptr), buffer_(path) {
    rdbuf(&buffer_);
}

MappedFileStream::~MappedFileStream() = default;

} // namespace dayo::core
