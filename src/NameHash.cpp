#include "NameHash.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace mctde {

uint32_t hashPath(const std::string& path) {
    uint32_t h = 0;
    for (char ch : path) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        h = h * 37u + c;
    }
    return h;
}

size_t NameMap::loadString(const char* data, size_t len) {
    size_t i = 0, count = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && data[j] != '\n') ++j;
        std::string line(data + i, j - i);
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty() && line[0] == '/') {  // skip comments/blanks
            byHash_[hashPath(line)] = line;
            ++count;
        }
        i = (j < len) ? j + 1 : j;
    }
    return count;
}

size_t NameMap::load(const std::string& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open namelist: " + file);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return loadString(text.data(), text.size());
}

size_t NameMap::loadEmbedded() {
    return loadString(reinterpret_cast<const char*>(kNamelistData), kNamelistLen);
}

const std::string* NameMap::find(uint32_t hash) const {
    auto it = byHash_.find(hash);
    return it == byHash_.end() ? nullptr : &it->second;
}

} // namespace mctde
