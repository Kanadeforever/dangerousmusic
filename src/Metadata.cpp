// 轻量元数据解析：支持常见 MP3 ID3、OGG Vorbis 注释、专辑字段和内嵌封面。
// 这里刻意不依赖第三方标签库，确保代理 DLL 只有 Windows 系统组件依赖。
#include "Metadata.h"
#include "Localization.h"

namespace localmusic {
namespace {

constexpr size_t kMaximumTagBytes = 16U * 1024U * 1024U;
constexpr size_t kMaximumArtworkBytes = 12U * 1024U * 1024U;

// 清理标签中的空字符、控制字符和多余首尾空白。
// 输出可直接用于日志和通知窗口，避免损坏标签破坏一行布局。
std::wstring CleanMetadataText(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
            continue;
        }
        if ((ch >= 0 && ch < 0x20) || ch == 0x7F) {
            ch = L' ';
        }
    }
    std::wstring collapsed;
    collapsed.reserve(value.size());
    bool previous_space = false;
    for (wchar_t ch : value) {
        const bool is_space = ch == L' ';
        if (is_space && previous_space) {
            continue;
        }
        collapsed.push_back(ch);
        previous_space = is_space;
    }
    return Trim(std::move(collapsed));
}

// 把 ID3 Latin-1 字节转换为 Unicode 宽字符串。
// 逐字节保留 0x80–0xFF 值，随后由清理函数移除无效控制字符。
std::wstring DecodeLatin1(const uint8_t* data, size_t length) {
    if (length == 0) {
        return {};
    }
    const int required = MultiByteToWideChar(28591, 0, reinterpret_cast<const char*>(data),
                                              static_cast<int>(length), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(28591, 0, reinterpret_cast<const char*>(data), static_cast<int>(length),
                        result.data(), required);
    return Trim(result);
}

// 解码大端 UTF-16 文本并处理字节序。
// 长度为奇数时忽略最后不完整字节，避免越界读取。
std::wstring DecodeUtf16Be(const uint8_t* data, size_t length) {
    if (length < 2) {
        return {};
    }
    std::wstring result;
    result.reserve(length / 2);
    for (size_t i = 0; i + 1 < length; i += 2) {
        result.push_back(static_cast<wchar_t>((static_cast<uint16_t>(data[i]) << 8) | data[i + 1]));
    }
    return Trim(result);
}

// 根据 ID3 文本帧首字节选择 Latin-1、UTF-16 或 UTF-8 解码。
// 不支持/损坏编码安全返回空文本，让后续侧挂元数据和文件名回退补缺。
std::wstring DecodeTextFrame(const uint8_t* data, size_t length) {
    if (length == 0) {
        return {};
    }
    const uint8_t encoding = data[0];
    ++data;
    --length;

    if (encoding == 0) {
        return DecodeLatin1(data, length);
    }
    if (encoding == 3) {
        return Trim(Utf8ToWide(std::string_view(reinterpret_cast<const char*>(data), length)));
    }
    if (encoding == 2) {
        return DecodeUtf16Be(data, length);
    }
    if (encoding == 1) {
        if (length >= 2 && data[0] == 0xFE && data[1] == 0xFF) {
            return DecodeUtf16Be(data + 2, length - 2);
        }
        size_t offset = 0;
        if (length >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
            offset = 2;
        }
        std::wstring result;
        for (size_t i = offset; i + 1 < length; i += 2) {
            const uint16_t value = static_cast<uint16_t>(data[i]) |
                                   (static_cast<uint16_t>(data[i + 1]) << 8);
            result.push_back(static_cast<wchar_t>(value));
        }
        return Trim(result);
    }
    return {};
}

// 读取“01/12”等标签中的首个正整数。
// 用于保留和显示曲号、碟号等元数据；非法值返回零表示未知。
int ParseLeadingInteger(const std::wstring& text) {
    const std::wstring value = Trim(text);
    if (value.empty()) return 0;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed <= 0 || parsed > 100000) return 0;
    return static_cast<int>(parsed);
}

// 从四字节网络序缓冲区读取无符号整数。
// 调用方保证长度，本辅助只集中处理移位与类型转换。
uint32_t ReadBigEndian32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

// 读取 ID3 使用的 7-bit synchsafe 32 位整数。
// 屏蔽每字节最高位，正确得到标签和帧长度。
uint32_t ReadSynchsafe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0] & 0x7F) << 21) |
           (static_cast<uint32_t>(data[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(data[2] & 0x7F) << 7) |
           static_cast<uint32_t>(data[3] & 0x7F);
}

// 验证并读取完整 ID3v2 标签区到内存。
// 对声明长度设置上限，防止损坏文件导致巨额分配。
bool ReadId3Tag(std::ifstream& file, uint8_t& version, std::vector<uint8_t>& data) {
    std::array<uint8_t, 10> header{};
    file.clear();
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    if (file.gcount() != static_cast<std::streamsize>(header.size()) ||
        header[0] != 'I' || header[1] != 'D' || header[2] != '3') {
        return false;
    }

    version = header[3];
    if (version != 3 && version != 4) {
        return false;
    }
    const uint32_t tag_size = ReadSynchsafe32(header.data() + 6);
    const size_t read_size = std::min<size_t>(tag_size, kMaximumTagBytes);
    data.resize(read_size);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    data.resize(static_cast<size_t>(std::max<std::streamsize>(0, file.gcount())));
    return !data.empty();
}

// 解析常见 ID3v2 文本帧并补充 TrackMetadata。
// 只处理项目需要的标题、艺术家、专辑、曲号等字段，未知帧安全跳过。
void ReadId3v2(std::ifstream& file, TrackMetadata& metadata) {
    uint8_t version = 0;
    std::vector<uint8_t> data;
    if (!ReadId3Tag(file, version, data)) {
        return;
    }

    size_t offset = 0;
    while (offset + 10 <= data.size()) {
        const char* id = reinterpret_cast<const char*>(data.data() + offset);
        if (id[0] == '\0') {
            break;
        }
        const uint32_t frame_size = version == 4 ? ReadSynchsafe32(data.data() + offset + 4)
                                                  : ReadBigEndian32(data.data() + offset + 4);
        if (frame_size == 0 || offset + 10ULL + frame_size > data.size()) {
            break;
        }
        const uint8_t* payload = data.data() + offset + 10;
        if (std::memcmp(id, "TIT2", 4) == 0) {
            metadata.title = DecodeTextFrame(payload, frame_size);
        } else if (std::memcmp(id, "TPE1", 4) == 0) {
            metadata.artist = DecodeTextFrame(payload, frame_size);
        } else if (std::memcmp(id, "TPE2", 4) == 0) {
            metadata.album_artist = DecodeTextFrame(payload, frame_size);
        } else if (std::memcmp(id, "TALB", 4) == 0) {
            metadata.album = DecodeTextFrame(payload, frame_size);
        } else if (std::memcmp(id, "TRCK", 4) == 0) {
            metadata.track_number = ParseLeadingInteger(DecodeTextFrame(payload, frame_size));
        } else if (std::memcmp(id, "TPOS", 4) == 0) {
            metadata.disc_number = ParseLeadingInteger(DecodeTextFrame(payload, frame_size));
        } else if (std::memcmp(id, "TDRC", 4) == 0 || std::memcmp(id, "TYER", 4) == 0) {
            metadata.year = DecodeTextFrame(payload, frame_size);
        } else if (std::memcmp(id, "TCON", 4) == 0) {
            metadata.genre = DecodeTextFrame(payload, frame_size);
        }
        offset += 10ULL + frame_size;
    }
}

// 从文件末尾读取传统 ID3v1 标签作为低优先级补缺。
// 不会覆盖已经从 ID3v2 获得的有效字段。
void ReadId3v1(std::ifstream& file, TrackMetadata& metadata) {
    file.clear();
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size < 128) {
        return;
    }
    std::array<uint8_t, 128> tag{};
    file.seekg(-128, std::ios::end);
    file.read(reinterpret_cast<char*>(tag.data()), tag.size());
    if (file.gcount() != static_cast<std::streamsize>(tag.size()) ||
        std::memcmp(tag.data(), "TAG", 3) != 0) {
        return;
    }
    if (metadata.title.empty()) {
        metadata.title = DecodeLatin1(tag.data() + 3, 30);
    }
    if (metadata.artist.empty()) {
        metadata.artist = DecodeLatin1(tag.data() + 33, 30);
    }
    if (metadata.album.empty()) {
        metadata.album = DecodeLatin1(tag.data() + 63, 30);
    }
}

// 读取文件开头有限字节供 OGG/Vorbis 松散扫描。
// 限制最大读取量，曲库扫描不会把整个大音频载入内存。
std::string ReadPrefix(std::ifstream& file, size_t maximum_bytes) {
    file.clear();
    file.seekg(0, std::ios::beg);
    std::vector<char> bytes(maximum_bytes);
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(std::max<std::streamsize>(0, file.gcount())));
    return std::string(bytes.begin(), bytes.end());
}

// 在 OGG 前缀中查找指定 Vorbis 键值。
// 用于兼容不同封装顺序；找不到时返回空字符串。
std::wstring FindLooseVorbisText(const std::string& blob, std::string_view key) {
    const size_t pos = blob.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    size_t begin = pos + key.size();
    size_t end = begin;
    while (end < blob.size() && blob[end] != '\0' && blob[end] != '\n' && blob[end] != '\r') {
        const unsigned char ch = static_cast<unsigned char>(blob[end]);
        if (ch < 0x20 && ch != '\t') {
            break;
        }
        ++end;
    }
    return Trim(Utf8ToWide(std::string_view(blob.data() + begin, end - begin)));
}

// 从 OGG/Vorbis 注释中提取标题、艺术家、专辑和编号。
// 按字段补缺，不覆盖更高优先级已解析值。
void ReadLooseVorbisComments(std::ifstream& file, TrackMetadata& metadata) {
    const std::string blob = ReadPrefix(file, 1024U * 1024U);
    if (metadata.title.empty()) {
        metadata.title = FindLooseVorbisText(blob, "TITLE=");
    }
    if (metadata.artist.empty()) {
        metadata.artist = FindLooseVorbisText(blob, "ARTIST=");
    }
    if (metadata.album.empty()) {
        metadata.album = FindLooseVorbisText(blob, "ALBUM=");
    }
    if (metadata.album_artist.empty()) {
        metadata.album_artist = FindLooseVorbisText(blob, "ALBUMARTIST=");
    }
    if (metadata.track_number == 0) {
        metadata.track_number = ParseLeadingInteger(FindLooseVorbisText(blob, "TRACKNUMBER="));
    }
    if (metadata.disc_number == 0) {
        metadata.disc_number = ParseLeadingInteger(FindLooseVorbisText(blob, "DISCNUMBER="));
    }
    if (metadata.year.empty()) metadata.year = FindLooseVorbisText(blob, "DATE=");
    if (metadata.genre.empty()) metadata.genre = FindLooseVorbisText(blob, "GENRE=");
}

// 判断文件名开头是否形如“01 - ”的曲号前缀。
// 真实专辑目录可移除该前缀，根目录保留数字艺术家的可能性。
bool LooksLikeTrackNumberPrefix(const std::wstring& text) {
    const std::wstring value = Trim(text);
    if (value.empty() || value.size() > 10) return false;
    bool has_digit = false;
    for (const wchar_t character : value) {
        if (std::iswdigit(character)) {
            has_digit = true;
            continue;
        }
        if (character == L' ' || character == L'.' || character == L'_' || character == L'-' ||
            character == L'[' || character == L']' || character == L'(' || character == L')') {
            continue;
        }
        return false;
    }
    return has_digit;
}

// 从文件名曲号前缀提取整数。
// 仅在 LooksLikeTrackNumberPrefix 成功后产生有效正数。
int TrackNumberFromPrefix(const std::wstring& text) {
    const std::wstring value = Trim(text);
    const wchar_t* cursor = value.c_str();
    while (*cursor && !std::iswdigit(*cursor)) ++cursor;
    if (!*cursor) return 0;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(cursor, &end, 10);
    return end != cursor && parsed > 0 && parsed <= 100000 ? static_cast<int>(parsed) : 0;
}

// 在内嵌标签和侧挂元数据缺失时，从“艺术家 - 曲名”或纯文件名生成基础元数据。
// 专辑目录中的纯数字左段可解释为曲号；根目录仍保留数字艺术家名称，避免误改真实文件名。
TrackMetadata FallbackFromFilename(const std::filesystem::path& path,
                                   bool numeric_prefix_is_track_number) {
    TrackMetadata metadata;
    const std::wstring stem = path.stem().wstring();
    const size_t separator = stem.find(L" - ");
    if (separator != std::wstring::npos) {
        const std::wstring left = Trim(stem.substr(0, separator));
        metadata.title = Trim(stem.substr(separator + 3));
        if (numeric_prefix_is_track_number && LooksLikeTrackNumberPrefix(left)) {
            metadata.track_number = TrackNumberFromPrefix(left);
        } else {
            metadata.artist = left;
        }
    } else {
        metadata.title = stem;
    }
    return metadata;
}

// 通过 PNG/JPEG/BMP/GIF 等魔数快速判断字节是否像图片。
// 拒绝明显错误的 APIC/COVERART 数据，避免把任意标签交给 GDI+ 解码。
bool LooksLikeImage(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
        return true;
    }
    if (bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return true;
    }
    if (bytes.size() >= 6 && (std::memcmp(bytes.data(), "GIF87a", 6) == 0 ||
                              std::memcmp(bytes.data(), "GIF89a", 6) == 0)) {
        return true;
    }
    if (bytes.size() >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
        return true;
    }
    return false;
}

// 解析 MP3 ID3 APIC 帧并返回首个可用内嵌封面。
// 验证 MIME、边界和图片魔数，损坏帧不会影响音乐播放。
TrackArtwork ReadId3Artwork(std::ifstream& file) {
    uint8_t version = 0;
    std::vector<uint8_t> data;
    if (!ReadId3Tag(file, version, data)) {
        return {};
    }

    size_t offset = 0;
    while (offset + 10 <= data.size()) {
        const char* id = reinterpret_cast<const char*>(data.data() + offset);
        if (id[0] == '\0') {
            break;
        }
        const uint32_t frame_size = version == 4 ? ReadSynchsafe32(data.data() + offset + 4)
                                                  : ReadBigEndian32(data.data() + offset + 4);
        if (frame_size == 0 || offset + 10ULL + frame_size > data.size()) {
            break;
        }
        if (std::memcmp(id, "APIC", 4) == 0 && frame_size >= 4) {
            const uint8_t* payload = data.data() + offset + 10;
            const size_t payload_size = frame_size;
            const uint8_t encoding = payload[0];
            size_t cursor = 1;
            const size_t mime_begin = cursor;
            while (cursor < payload_size && payload[cursor] != 0) {
                ++cursor;
            }
            if (cursor >= payload_size) {
                return {};
            }
            const std::string mime(reinterpret_cast<const char*>(payload + mime_begin), cursor - mime_begin);
            ++cursor;
            if (cursor >= payload_size) {
                return {};
            }
            ++cursor;  // 图片类型。

            // 描述字段的终止符取决于 ID3 文本编码。
            if (encoding == 1 || encoding == 2) {
                while (cursor + 1 < payload_size && !(payload[cursor] == 0 && payload[cursor + 1] == 0)) {
                    cursor += 2;
                }
                cursor = std::min(payload_size, cursor + 2);
            } else {
                while (cursor < payload_size && payload[cursor] != 0) {
                    ++cursor;
                }
                cursor = std::min(payload_size, cursor + 1);
            }
            if (cursor >= payload_size || payload_size - cursor > kMaximumArtworkBytes) {
                return {};
            }
            TrackArtwork artwork;
            artwork.bytes.assign(payload + cursor, payload + payload_size);
            artwork.mime_type = Utf8ToWide(mime);
            if (LooksLikeImage(artwork.bytes)) {
                return artwork;
            }
            return {};
        }
        offset += 10ULL + frame_size;
    }
    return {};
}

// 把一个 Base64 字符转换为 0–63，非法字符返回负值。
// 供 OGG METADATA_BLOCK_PICTURE/COVERART 解码使用。
int Base64Value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

// 容忍空白地解码 Base64 文本。
// 格式非法时返回空数组，封面缺失不会上升为播放错误。
std::vector<uint8_t> DecodeBase64(std::string_view text) {
    std::vector<uint8_t> output;
    output.reserve(text.size() * 3 / 4);
    uint32_t accumulator = 0;
    int bits = 0;
    for (unsigned char ch : text) {
        if (ch == '=') {
            break;
        }
        const int value = Base64Value(ch);
        if (value < 0) {
            continue;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xFFU));
            if (output.size() > kMaximumArtworkBytes) {
                return {};
            }
        }
    }
    return output;
}

// 在二进制前缀中查找指定键后的 Base64 文本范围。
// 返回 string_view 指向原缓冲区，调用方必须在缓冲区生命周期内使用。
std::string_view FindLooseBase64Value(const std::string& blob, std::string_view key) {
    const size_t pos = blob.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    size_t begin = pos + key.size();
    size_t end = begin;
    while (end < blob.size()) {
        const unsigned char ch = static_cast<unsigned char>(blob[end]);
        if (Base64Value(ch) < 0 && ch != '=') {
            break;
        }
        ++end;
    }
    return std::string_view(blob.data() + begin, end - begin);
}

// 解析 FLAC picture block 结构并提取图片 MIME 与字节。
// 逐段检查长度，任何越界声明都会安全失败。
TrackArtwork ParseFlacPictureBlock(const std::vector<uint8_t>& block) {
    size_t cursor = 0;
    auto read_u32 = [&](uint32_t& value) -> bool {
        if (cursor + 4 > block.size()) return false;
        value = ReadBigEndian32(block.data() + cursor);
        cursor += 4;
        return true;
    };

    uint32_t picture_type = 0;
    uint32_t mime_length = 0;
    if (!read_u32(picture_type) || !read_u32(mime_length) || cursor + mime_length > block.size()) {
        return {};
    }
    (void)picture_type;
    const std::string mime(reinterpret_cast<const char*>(block.data() + cursor), mime_length);
    cursor += mime_length;

    uint32_t description_length = 0;
    if (!read_u32(description_length) || cursor + description_length > block.size()) {
        return {};
    }
    cursor += description_length;

    uint32_t ignored = 0;
    for (int i = 0; i < 4; ++i) {
        if (!read_u32(ignored)) return {};
    }
    uint32_t data_length = 0;
    if (!read_u32(data_length) || data_length == 0 || data_length > kMaximumArtworkBytes ||
        cursor + data_length > block.size()) {
        return {};
    }

    TrackArtwork artwork;
    artwork.bytes.assign(block.begin() + static_cast<std::ptrdiff_t>(cursor),
                         block.begin() + static_cast<std::ptrdiff_t>(cursor + data_length));
    artwork.mime_type = Utf8ToWide(mime);
    return LooksLikeImage(artwork.bytes) ? artwork : TrackArtwork{};
}

// 从 OGG 注释中的 METADATA_BLOCK_PICTURE 或 COVERART 读取封面。
// 优先标准 picture block，兼容旧式纯 Base64 图片。
TrackArtwork ReadOggArtwork(std::ifstream& file) {
    const std::string blob = ReadPrefix(file, 8U * 1024U * 1024U);
    if (const std::string_view encoded = FindLooseBase64Value(blob, "METADATA_BLOCK_PICTURE="); !encoded.empty()) {
        const std::vector<uint8_t> block = DecodeBase64(encoded);
        if (TrackArtwork artwork = ParseFlacPictureBlock(block); !artwork.Empty()) {
            return artwork;
        }
    }
    if (const std::string_view encoded = FindLooseBase64Value(blob, "COVERART="); !encoded.empty()) {
        TrackArtwork artwork;
        artwork.bytes = DecodeBase64(encoded);
        artwork.mime_type = FindLooseVorbisText(blob, "COVERARTMIME=");
        if (LooksLikeImage(artwork.bytes)) {
            return artwork;
        }
    }
    return {};
}


// 识别 UTF-8 BOM、UTF-16LE BOM 或无 BOM 文本并转为宽字符串。
// 同名元数据 INI 可安全包含中日韩字符。
std::wstring DecodeIniText(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring text;
        text.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
            text.push_back(static_cast<wchar_t>(static_cast<uint16_t>(bytes[i]) |
                                                (static_cast<uint16_t>(bytes[i + 1]) << 8)));
        }
        return text;
    }
    size_t offset = bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
    return Utf8ToWide(std::string_view(reinterpret_cast<const char*>(bytes.data() + offset),
                                       bytes.size() - offset));
}

using SimpleIniValues = std::unordered_map<std::wstring, std::wstring>;

// 读取曲目/专辑侧挂 INI 为小型键值结构。
// 只实现本项目所需语法，不调用系统 INI API以便处理任意音乐目录文件。
SimpleIniValues ParseSimpleIni(const std::filesystem::path& path) {
    SimpleIniValues values;
    std::ifstream file(path, std::ios::binary);
    if (!file) return values;
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0 || size > 1024 * 1024) return values;
    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(std::max<std::streamsize>(0, file.gcount())));

    std::wistringstream input(DecodeIniText(bytes));
    std::wstring section;
    std::wstring line;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            section = ToLower(Trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        const std::wstring key = ToLower(Trim(line.substr(0, equals)));
        std::wstring value = Trim(line.substr(equals + 1));
        if (!section.empty() && !key.empty()) values[section + L"." + key] = std::move(value);
    }
    return values;
}

// 从同名曲目 INI 或文件夹 album.ini 读取一个文本字段，并清理控制字符。
// 线程局部缓存按“文件路径 + 最后写入时间”失效，扫描同一文件的多个字段时只解析一次。
std::wstring ReadProfileText(const std::filesystem::path& path, const wchar_t* section,
                             const wchar_t* key) {
    if (path.empty()) return {};
    // 扫描同一首曲目时会连续读取多个字段。线程局部缓存避免为每个字段
    // 重新打开一次侧挂 INI，同时不引入跨线程共享状态。
    thread_local std::filesystem::path cached_path;
    thread_local std::filesystem::file_time_type cached_write_time{};
    thread_local bool cached_time_valid = false;
    thread_local SimpleIniValues cached_values;
    std::error_code time_error;
    const auto write_time = std::filesystem::last_write_time(path, time_error);
    const bool time_valid = !time_error;
    if (cached_path != path || cached_time_valid != time_valid ||
        (time_valid && write_time != cached_write_time)) {
        cached_path = path;
        cached_write_time = write_time;
        cached_time_valid = time_valid;
        cached_values = ParseSimpleIni(path);
    }
    const std::wstring lookup = ToLower(std::wstring(section ? section : L"")) + L"." +
                                ToLower(std::wstring(key ? key : L""));
    const auto found = cached_values.find(lookup);
    return found == cached_values.end() ? std::wstring{} : CleanMetadataText(found->second);
}

// 读取侧挂 INI 中的布尔字段，兼容 1/0、true/false、yes/no 与 on/off。
// 缺失或拼写无法识别时返回调用方提供的 fallback，不让可选元数据阻断曲库扫描。
bool ReadProfileFlag(const std::filesystem::path& path, const wchar_t* section,
                     const wchar_t* key, bool fallback) {
    const std::wstring value = ToLower(Trim(ReadProfileText(path, section, key)));
    if (value.empty()) return fallback;
    if (value == L"1" || value == L"true" || value == L"yes" || value == L"on") return true;
    if (value == L"0" || value == L"false" || value == L"no" || value == L"off") return false;
    return fallback;
}

// 把元数据文本转换为正整数，零和负数视为未知。
// 用于读取 TrackNumber、DiscNumber 等显示字段；异常值统一视为未知。
int ParsePositiveNumber(const std::wstring& value) {
    if (value.empty()) return 0;
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed <= 0 || parsed > 100000) return 0;
    return static_cast<int>(parsed);
}

// 把 INI 中的封面路径解析为实际文件：相对路径以当前曲目/专辑目录为基准。
// 只有候选确实是普通文件时才返回规范化路径，缺失、目录或无效路径统一返回空。
std::filesystem::path ExistingRelativePath(const std::filesystem::path& base,
                                           const std::wstring& value) {
    if (value.empty()) return {};
    std::filesystem::path candidate(value);
    if (candidate.is_relative()) candidate = base / candidate;
    std::error_code ec;
    return std::filesystem::is_regular_file(candidate, ec) ? candidate.lexically_normal()
                                                            : std::filesystem::path{};
}

// 在指定目录内按固定扩展名顺序查找“同主文件名”的外部图片。
// 只接受实际普通文件，并返回第一张匹配的 PNG/JPG/JPEG/BMP，保证扫描结果稳定。
std::filesystem::path FindImageWithStem(const std::filesystem::path& directory,
                                        const std::wstring& stem) {
    static constexpr const wchar_t* kExtensions[] = {L".png", L".jpg", L".jpeg", L".bmp"};
    for (const wchar_t* extension : kExtensions) {
        const std::filesystem::path candidate = directory / (stem + extension);
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

// 按 cover/folder/front 与常见图片扩展名查找文件夹专辑封面。
// 候选顺序固定，保证同一目录每次扫描选择一致。
std::filesystem::path FindAlbumCover(const std::filesystem::path& directory) {
    static constexpr const wchar_t* kNames[] = {L"cover", L"folder", L"front"};
    for (const wchar_t* name : kNames) {
        if (const std::filesystem::path found = FindImageWithStem(directory, name); !found.empty()) {
            return found;
        }
    }
    return {};
}

// 受大小上限保护地读取外部封面文件。
// 读取后验证图片魔数；错误图片不会交给通知线程解码。
TrackArtwork ReadExternalArtworkFile(const std::filesystem::path& path) {
    if (path.empty()) return {};
    std::error_code ec;
    const uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size == 0 || file_size > kMaximumArtworkBytes) return {};

    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    TrackArtwork artwork;
    artwork.bytes.resize(static_cast<size_t>(file_size));
    file.read(reinterpret_cast<char*>(artwork.bytes.data()),
              static_cast<std::streamsize>(artwork.bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(artwork.bytes.size()) ||
        !LooksLikeImage(artwork.bytes)) {
        return {};
    }
    const std::wstring extension = ToLower(path.extension().wstring());
    if (extension == L".png") artwork.mime_type = L"image/png";
    else if (extension == L".jpg" || extension == L".jpeg") artwork.mime_type = L"image/jpeg";
    else if (extension == L".bmp") artwork.mime_type = L"image/bmp";
    return artwork;
}

// 按“仅补缺或允许覆盖”规则合并一个文本字段。
// 侧挂元数据和 album.ini 共用该规则，确保优先级可预测。
void FillMissing(std::wstring& destination, const std::wstring& value, bool override_existing) {
    if (!value.empty() && (override_existing || destination.empty())) destination = value;
}

}  // namespace

// 读取文件夹 album.ini、目录名和专辑封面，生成专辑级回退信息。
// 音乐根目录会标记为未分类虚拟专辑，避免误把根目录名当普通专辑。
AlbumMetadata ReadAlbumMetadata(const std::filesystem::path& directory,
                                const std::filesystem::path& music_root) {
    AlbumMetadata metadata;
    const std::filesystem::path album_ini = directory / L"album.ini";
    metadata.title = ReadProfileText(album_ini, L"Album", L"Title");
    metadata.artist = ReadProfileText(album_ini, L"Album", L"Artist");
    metadata.album_artist = ReadProfileText(album_ini, L"Album", L"AlbumArtist");
    metadata.year = ReadProfileText(album_ini, L"Album", L"Year");
    metadata.genre = ReadProfileText(album_ini, L"Album", L"Genre");
    metadata.disc_number = ParsePositiveNumber(ReadProfileText(album_ini, L"Album", L"DiscNumber"));

    const std::wstring cover_value = ReadProfileText(album_ini, L"Album", L"Cover");
    metadata.cover_path = ExistingRelativePath(directory, cover_value);
    if (metadata.cover_path.empty()) metadata.cover_path = FindAlbumCover(directory);

    std::error_code ec;
    const std::filesystem::path normalized_directory = std::filesystem::weakly_canonical(directory, ec);
    ec.clear();
    const std::filesystem::path normalized_root = std::filesystem::weakly_canonical(music_root, ec);
    const bool is_root = !normalized_directory.empty() && !normalized_root.empty()
        ? normalized_directory == normalized_root
        : directory.lexically_normal() == music_root.lexically_normal();
    metadata.is_music_root = is_root;
    if (metadata.title.empty()) {
        metadata.title = is_root ? loc::Text(L"Metadata.Uncategorized") : directory.filename().wstring();
    }
    if (metadata.album_artist.empty()) metadata.album_artist = metadata.artist;
    if (metadata.artist.empty()) metadata.artist = metadata.album_artist;
    metadata.title = CleanMetadataText(std::move(metadata.title));
    metadata.artist = CleanMetadataText(std::move(metadata.artist));
    metadata.album_artist = CleanMetadataText(std::move(metadata.album_artist));
    metadata.year = CleanMetadataText(std::move(metadata.year));
    metadata.genre = CleanMetadataText(std::move(metadata.genre));
    return metadata;
}

// 综合内嵌标签、同名 INI、文件名和专辑信息生成最终曲目元数据。
// 逐字段补缺并记录外部封面路径，扫描线程不解码图片。
TrackMetadata ReadTrackMetadata(const std::filesystem::path& path,
                                const AlbumMetadata* album,
                                bool enable_sidecar,
                                bool sidecar_override) {
    TrackMetadata metadata;
    std::ifstream file(path, std::ios::binary);
    if (file) {
        const std::wstring extension = ToLower(path.extension().wstring());
        if (extension == L".mp3") {
            ReadId3v2(file, metadata);
            ReadId3v1(file, metadata);
        } else if (extension == L".ogg") {
            ReadLooseVorbisComments(file, metadata);
        }
    }

    const std::filesystem::path sidecar = path.parent_path() / (path.stem().wstring() + L".ini");
    if (enable_sidecar) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(sidecar, ec)) {
            const bool override_existing = sidecar_override ||
                ReadProfileFlag(sidecar, L"Track", L"OverrideEmbeddedMetadata", false);
            FillMissing(metadata.title, ReadProfileText(sidecar, L"Track", L"Title"), override_existing);
            FillMissing(metadata.artist, ReadProfileText(sidecar, L"Track", L"Artist"), override_existing);
            FillMissing(metadata.album, ReadProfileText(sidecar, L"Track", L"Album"), override_existing);
            FillMissing(metadata.album_artist, ReadProfileText(sidecar, L"Track", L"AlbumArtist"), override_existing);
            FillMissing(metadata.year, ReadProfileText(sidecar, L"Track", L"Year"), override_existing);
            FillMissing(metadata.genre, ReadProfileText(sidecar, L"Track", L"Genre"), override_existing);
            const int track_number = ParsePositiveNumber(ReadProfileText(sidecar, L"Track", L"TrackNumber"));
            const int disc_number = ParsePositiveNumber(ReadProfileText(sidecar, L"Track", L"DiscNumber"));
            if (track_number > 0 && (override_existing || metadata.track_number == 0)) metadata.track_number = track_number;
            if (disc_number > 0 && (override_existing || metadata.disc_number == 0)) metadata.disc_number = disc_number;
            metadata.sidecar_cover_path = ExistingRelativePath(
                path.parent_path(), ReadProfileText(sidecar, L"Track", L"Cover"));
        }
    }

    // 与音频文件同主文件名的图片属于曲目自己的外部封面。
    metadata.track_cover_path = FindImageWithStem(path.parent_path(), path.stem().wstring());

    const bool numeric_prefix_is_track_number = album && !album->is_music_root;
    const TrackMetadata fallback = FallbackFromFilename(path, numeric_prefix_is_track_number);
    if (metadata.title.empty()) metadata.title = fallback.title;
    if (metadata.artist.empty() && !fallback.artist.empty()) metadata.artist = fallback.artist;
    if (metadata.track_number == 0 && fallback.track_number > 0) {
        metadata.track_number = fallback.track_number;
    }

    if (album) {
        if (metadata.album.empty()) metadata.album = album->title;
        if (metadata.album_artist.empty()) metadata.album_artist = album->album_artist;
        if (metadata.artist.empty()) {
            metadata.artist = !album->artist.empty() ? album->artist : album->album_artist;
        }
        if (metadata.year.empty()) metadata.year = album->year;
        if (metadata.genre.empty()) metadata.genre = album->genre;
        if (metadata.disc_number == 0) metadata.disc_number = album->disc_number;
        metadata.album_cover_path = album->cover_path;
    }
    if (metadata.artist.empty()) metadata.artist = loc::Text(L"Metadata.LocalMusic");

    metadata.title = CleanMetadataText(std::move(metadata.title));
    metadata.artist = CleanMetadataText(std::move(metadata.artist));
    metadata.album = CleanMetadataText(std::move(metadata.album));
    metadata.album_artist = CleanMetadataText(std::move(metadata.album_artist));
    metadata.year = CleanMetadataText(std::move(metadata.year));
    metadata.genre = CleanMetadataText(std::move(metadata.genre));
    return metadata;
}

// 按文件扩展名读取 MP3 或 OGG 内嵌封面。
// WAV 等不支持格式直接返回空，让同名图片/专辑封面接管。
TrackArtwork ReadEmbeddedArtwork(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    const std::wstring extension = ToLower(path.extension().wstring());
    if (extension == L".mp3") return ReadId3Artwork(file);
    if (extension == L".ogg") return ReadOggArtwork(file);
    return {};
}

// 按配置和优先级选择内嵌、同名、侧挂指定或专辑封面。
// 实际字节读取发生在通知线程，不阻塞 FMOD 播放状态机。
TrackArtwork ReadTrackArtwork(const std::filesystem::path& path,
                              const TrackMetadata& metadata,
                              bool allow_embedded,
                              bool allow_external,
                              bool album_only) {
    if (album_only) return allow_external ? ReadExternalArtworkFile(metadata.album_cover_path) : TrackArtwork{};
    if (allow_embedded) {
        if (TrackArtwork embedded = ReadEmbeddedArtwork(path); !embedded.Empty()) return embedded;
    }
    if (!allow_external) return {};
    if (TrackArtwork external = ReadExternalArtworkFile(metadata.track_cover_path); !external.Empty()) return external;
    if (TrackArtwork explicit_cover = ReadExternalArtworkFile(metadata.sidecar_cover_path); !explicit_cover.Empty()) {
        return explicit_cover;
    }
    return ReadExternalArtworkFile(metadata.album_cover_path);
}

}  // namespace localmusic
