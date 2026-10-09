#include "SpurSourceFiles.h"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace osvm {

namespace fs = std::filesystem;

uint32_t SpurSourceFiles::fileIndexOf(uint64_t sourcePointer) {
    if (sourcePointer == 0) return 0;
    return ((sourcePointer - POINTER_BASE) & CHANGES_FLAG) != 0 ? 2 : 1;
}

uint64_t SpurSourceFiles::filePositionOf(uint64_t sourcePointer) {
    if (sourcePointer == 0) return 0;
    return ((sourcePointer - POINTER_BASE) & POSITION_MASK) * POINTER_SCALE;
}

bool SpurSourceFiles::readWhole(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(in.read(out.data(), size));
}

bool SpurSourceFiles::loadSources(const std::string& path) {
    if (!readWhole(path, sources)) return false;
    sourcesFile = path;
    return true;
}

bool SpurSourceFiles::loadChanges(const std::string& path) {
    if (!readWhole(path, changes)) return false;
    changesFile = path;
    return true;
}

void SpurSourceFiles::locateFor(const std::string& imagePath) {
    const fs::path image(imagePath);
    const fs::path dir = image.parent_path();

    fs::path candidate = image;
    candidate.replace_extension(".changes");
    if (fs::exists(candidate)) loadChanges(candidate.string());

    // The .sources file is shared across point releases, so it is matched by
    // extension rather than by stem; take the newest when several exist.
    std::error_code ec;
    fs::path newest;
    fs::file_time_type newestTime{};
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() != ".sources") continue;
        const auto stamp = entry.last_write_time(ec);
        if (newest.empty() || stamp > newestTime) {
            newest = entry.path();
            newestTime = stamp;
        }
    }
    if (!newest.empty()) loadSources(newest.string());
}

std::string SpurSourceFiles::chunkAt(uint64_t sourcePointer) const {
    const uint32_t index = fileIndexOf(sourcePointer);
    if (index == 0) return {};
    return chunkIn(index, filePositionOf(sourcePointer));
}

std::string SpurSourceFiles::chunkIn(uint32_t fileIndex, uint64_t position) const {
    const std::string& buffer = fileIndex == 2 ? changes : sources;
    if (fileIndex == 0 || buffer.empty()) return {};
    if (position >= buffer.size()) return {};

    // A chunk runs to the next bang; a doubled bang is an escaped one.
    size_t cursor = static_cast<size_t>(position);
    std::string text;
    while (cursor < buffer.size()) {
        const size_t bang = buffer.find('!', cursor);
        if (bang == std::string::npos) {
            text.append(buffer, cursor, buffer.size() - cursor);
            break;
        }
        if (bang + 1 < buffer.size() && buffer[bang + 1] == '!') {
            text.append(buffer, cursor, bang + 1 - cursor);
            cursor = bang + 2;
            continue;
        }
        text.append(buffer, cursor, bang - cursor);
        break;
    }

    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    return text;
}

} // namespace osvm
