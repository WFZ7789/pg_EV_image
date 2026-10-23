/// SPDX-License-Identifier: MIT

#include "sensors/input.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>

namespace fy::sensors {
namespace {

std::uint32_t little(std::span<const unsigned char> bytes,std::size_t offset,std::size_t count) {
    if (offset+count > bytes.size()) throw std::runtime_error("Truncated ZIP header");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) value |= std::uint32_t(bytes[offset+i]) << (i*8);
    return value;
}

void read(std::ifstream& input,std::span<unsigned char> bytes) {
    input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("Truncated ZIP data");
}

struct Member {
    std::string name;
    std::uint32_t compressed = 0,size = 0,crc = 0,offset = 0;
    unsigned method = 0;
};

Member netcdf_member(std::ifstream& input) {
    input.seekg(0,std::ios::end);
    const auto length = input.tellg();
    if (length < 22) throw std::runtime_error("Invalid ZIP size");
    const auto tail_size = std::min<std::streamoff>(length,65557);
    std::vector<unsigned char> tail(static_cast<std::size_t>(tail_size));
    input.seekg(length-tail_size); read(input,tail);
    std::size_t end = tail.size()-22;
    while (little(tail,end,4) != 0x06054b50 || end+22+little(tail,end+20,2) != tail.size()) {
        if (!end) throw std::runtime_error("Missing ZIP central directory");
        --end;
    }
    const auto count = little(tail,end+10,2),offset = little(tail,end+16,4);
    if (count == 65535 || offset == 0xffffffff) throw std::runtime_error("ZIP64 is not supported");
    input.seekg(offset);
    Member selected;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::array<unsigned char,46> header{}; read(input,header);
        if (little(header,0,4) != 0x02014b50) throw std::runtime_error("Invalid ZIP central directory");
        const auto name_size = little(header,28,2),extra = little(header,30,2),comment = little(header,32,2);
        std::string name(name_size,'\0'); input.read(name.data(),name.size());
        input.seekg(extra+comment,std::ios::cur);
        const auto basename = std::filesystem::path(name).filename().string();
        if (!name.ends_with(".nc") || basename.starts_with("._")) continue;
        if (!selected.name.empty()) throw std::runtime_error("ZIP must contain one NetCDF data file");
        if (little(header,8,2)&1) throw std::runtime_error("Encrypted ZIP is not supported");
        selected = {basename,little(header,20,4),little(header,24,4),little(header,16,4),
                    little(header,42,4),little(header,10,2)};
        if (selected.method != 0 && selected.method != 8) throw std::runtime_error("Unsupported ZIP compression");
    }
    if (selected.name.empty()) throw std::runtime_error("ZIP contains no NetCDF data file");
    return selected;
}

void extract(std::ifstream& input,const Member& member,const std::filesystem::path& destination) {
    input.seekg(member.offset);
    std::array<unsigned char,30> header{}; read(input,header);
    if (little(header,0,4) != 0x04034b50) throw std::runtime_error("Invalid ZIP local header");
    input.seekg(little(header,26,2)+little(header,28,2),std::ios::cur);
    std::ofstream output(destination,std::ios::binary);
    output.exceptions(std::ios::badbit|std::ios::failbit);
    std::array<unsigned char,65536> encoded{},decoded{};
    z_stream stream{};
    if (member.method == 8 && inflateInit2(&stream,-MAX_WBITS) != Z_OK)
        throw std::runtime_error("ZIP inflate initialization failed");
    struct End {
        z_stream& stream;
        bool active;
        ~End() { if (active) inflateEnd(&stream); }
    } end{stream,member.method == 8};
    std::uint64_t written = 0;
    uLong crc = crc32(0,Z_NULL,0);
    auto emit = [&](const unsigned char* bytes,std::size_t size) {
        written += size;
        if (written > member.size) throw std::runtime_error("ZIP decoded size exceeds header");
        output.write(reinterpret_cast<const char*>(bytes),static_cast<std::streamsize>(size));
        crc = crc32(crc,bytes,static_cast<uInt>(size));
    };
    std::uint32_t remaining = member.compressed;
    int status = Z_OK;
    while (remaining) {
        const auto size = std::min<std::size_t>(encoded.size(),remaining);
        read(input,std::span(encoded).first(size)); remaining -= size;
        if (member.method == 0) emit(encoded.data(),size);
        else {
            stream.next_in = encoded.data(); stream.avail_in = static_cast<uInt>(size);
            do {
                stream.next_out = decoded.data(); stream.avail_out = decoded.size();
                status = inflate(&stream,Z_NO_FLUSH);
                if (status != Z_OK && status != Z_STREAM_END) throw std::runtime_error("ZIP inflate failed");
                emit(decoded.data(),decoded.size()-stream.avail_out);
            } while (status != Z_STREAM_END && (stream.avail_in || stream.avail_out == 0));
            if (status == Z_STREAM_END && (remaining || stream.avail_in)) throw std::runtime_error("ZIP trailing compressed data");
        }
    }
    if ((member.method == 8 && status != Z_STREAM_END) || written != member.size || crc != member.crc)
        throw std::runtime_error("ZIP size or CRC mismatch");
}

}

std::filesystem::path resolve_data_input(const std::filesystem::path& path,const core::ScratchDirectory& scratch) {
    auto extension = path.extension().string();
    std::ranges::transform(extension,extension.begin(),[](unsigned char c) { return char(std::tolower(c)); });
    if (extension != ".zip") return path;
    const auto key = std::hash<std::string>{}(std::filesystem::absolute(path).lexically_normal().string());
    const auto directory = scratch.path()/("input-"+std::to_string(key));
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open ZIP: "+path.string());
    const auto member = netcdf_member(input);
    const auto destination = directory/member.name;
    if (std::filesystem::exists(destination)) return destination;
    std::filesystem::create_directories(directory);
    try { extract(input,member,destination); }
    catch (...) { std::error_code error; std::filesystem::remove(destination,error); throw; }
    return destination;
}

}
