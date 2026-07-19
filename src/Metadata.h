// 曲目/专辑元数据与封面读取接口。
//
// 优先级约定：
// 1. 音频文件自己的内嵌标签；
// 2. 与音频同主文件名的 INI 侧挂元数据（默认只补缺，可显式覆盖）；
// 3. 所属文件夹的 album.ini；
// 4. 文件名与文件夹名称回退。
//
// 封面优先级：内嵌封面 -> 同名图片 -> 曲目 INI 指定图片 -> 专辑封面。
#pragma once
#include "Common.h"

namespace localmusic {

struct AlbumMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album_artist;
    std::wstring year;
    std::wstring genre;
    int disc_number = 0;
    // 标记音乐根目录的“未分类曲目”虚拟专辑。文件名中的纯数字前缀
    // 在真实专辑目录里可解释为曲号，在根目录则保留数字艺术家的可能性。
    bool is_music_root = false;
    std::filesystem::path cover_path;
};

struct TrackMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring album_artist;
    std::wstring year;
    std::wstring genre;
    int track_number = 0;
    int disc_number = 0;

    // 以下路径只保存封面来源，不在扫描阶段载入图片字节。
    // 真正读取与解码发生在通知线程中，避免阻塞 FMOD 切歌。
    std::filesystem::path track_cover_path;
    std::filesystem::path sidecar_cover_path;
    std::filesystem::path album_cover_path;
};

struct TrackArtwork {
    std::vector<uint8_t> bytes;
    std::wstring mime_type;

    // 判断封面字节数组是否为空。
    // 不解析 MIME，只用于快速决定是否需要 GDI+ 解码。
    bool Empty() const { return bytes.empty(); }
};

// 读取文件夹专辑级元数据与封面路径。
// 目录没有 album.ini 时使用文件夹名称和标准封面文件名回退。
AlbumMetadata ReadAlbumMetadata(const std::filesystem::path& directory,
                                const std::filesystem::path& music_root);
// 读取单曲内嵌标签、同名 INI、文件名和专辑补缺信息。
// 只保存封面来源路径，不在扫描线程解码图片。
TrackMetadata ReadTrackMetadata(const std::filesystem::path& path,
                                const AlbumMetadata* album = nullptr,
                                bool enable_sidecar = true,
                                bool sidecar_override = false);
// 读取 MP3/OGG 内嵌封面。
// 不支持或损坏时返回空结果。
TrackArtwork ReadEmbeddedArtwork(const std::filesystem::path& path);
// 按配置优先级选择并读取最终通知封面。
// 可强制只使用专辑级封面以显示专辑切换通知。
TrackArtwork ReadTrackArtwork(const std::filesystem::path& path,
                              const TrackMetadata& metadata,
                              bool allow_embedded,
                              bool allow_external,
                              bool album_only = false);

}  // namespace localmusic
