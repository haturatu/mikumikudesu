#include "core/mapped_file.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dayo::core {

MappedFileStream::Buffer::Buffer(const std::filesystem::path& path)
#if defined(__linux__)
    : fileDescriptor_(::open(path.c_str(), O_RDONLY | O_CLOEXEC))
#endif
{
#if defined(__linux__)
    if (fileDescriptor_ >= 0) {
        struct stat metadata {};
        if (::fstat(fileDescriptor_, &metadata) == 0 && metadata.st_size > 0) {
            mappedSize_ = static_cast<std::size_t>(metadata.st_size);
            void* address = ::mmap(nullptr, mappedSize_, PROT_READ, MAP_PRIVATE, fileDescriptor_, 0);
            if (address != MAP_FAILED) {
                mapped_ = static_cast<const char*>(address);
                setg(const_cast<char*>(mapped_), const_cast<char*>(mapped_), const_cast<char*>(mapped_ + mappedSize_));
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

std::size_t MappedFileStream::Buffer::size() const noexcept {
    return mapped_ != nullptr ? mappedSize_ : fallback_.size();
}

std::size_t MappedFileStream::Buffer::remaining() const noexcept {
    if (eback() == nullptr || gptr() == nullptr)
        return size();
    const auto position = static_cast<std::size_t>(gptr() - eback());
    return std::min(position, size()) < size() ? size() - position : 0;
}

MappedFileStream::Buffer::pos_type MappedFileStream::Buffer::seekoff(off_type off, std::ios_base::seekdir way,
                                                                     std::ios_base::openmode which) {
    if (which != std::ios_base::in || size() > static_cast<std::size_t>(std::numeric_limits<off_type>::max()))
        return pos_type(off_type(-1));
    const auto end = static_cast<off_type>(size());
    const auto current = gptr() == nullptr ? off_type{} : static_cast<off_type>(gptr() - eback());
    off_type base{};
    switch (way) {
    case std::ios_base::beg:
        break;
    case std::ios_base::cur:
        base = current;
        break;
    case std::ios_base::end:
        base = end;
        break;
    default:
        return pos_type(off_type(-1));
    }
    if (off > end - base || off < -base)
        return pos_type(off_type(-1));
    const auto position = base + off;
    setg(eback(), eback() == nullptr ? nullptr : eback() + position, egptr());
    return pos_type(position);
}

MappedFileStream::Buffer::pos_type MappedFileStream::Buffer::seekpos(pos_type position, std::ios_base::openmode which) {
    if (which != std::ios_base::in || size() > static_cast<std::size_t>(std::numeric_limits<off_type>::max()))
        return pos_type(off_type(-1));
    const auto end = static_cast<off_type>(size());
    if (position < pos_type(0) || position > pos_type(end))
        return pos_type(off_type(-1));
    const auto offset = static_cast<off_type>(position);
    setg(eback(), eback() == nullptr ? nullptr : eback() + offset, egptr());
    return position;
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

std::size_t MappedFileStream::remaining() const noexcept {
    return buffer_.remaining();
}

} // namespace dayo::core
