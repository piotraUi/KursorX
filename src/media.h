#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

struct IMFPMediaPlayer;
class VideoCallback;
namespace Gdiplus { class Image; }

enum class MediaKind { Unknown, Image, Video };
MediaKind KindOf(const std::wstring& path);

enum FitMode { kFill = 0, kFit = 1, kStretch = 2 };

// Odtwarza obraz / GIF (GDI+) albo film (Media Foundation) w podanym oknie.
// Uzywane zarowno do podgladu w aplikacji, jak i do tapety na pulpicie.
class MediaView {
public:
    MediaView() = default;
    MediaView(const MediaView&) = delete;
    MediaView& operator=(const MediaView&) = delete;
    ~MediaView() { Close(); }

    bool Open(HWND host, const std::wstring& path, bool mute, int fit);
    void Close();
    bool IsOpen() const { return host_ != nullptr; }
    void SetPaused(bool paused);
    void SetMute(bool mute);
    void SetFit(int fit);
    std::wstring Info() const { return info_; }

    // wywolywane z procedury okna
    void Paint(HDC dc);
    void OnTimer();
    void OnSize();
    void UpdateVideo();
    // wywolywane przez VideoCallback
    void OnVideoReady();
    void OnVideoEnded();

private:
    void ScheduleFrame();
    void LayoutVideo();

    HWND host_ = nullptr;
    HWND videoWnd_ = nullptr;
    std::wstring path_;
    std::wstring info_;
    int fit_ = kFill;
    bool mute_ = true;
    bool paused_ = false;

    std::unique_ptr<Gdiplus::Image> img_;
    unsigned frames_ = 1, frame_ = 0;
    std::vector<int> delays_;

    IMFPMediaPlayer* player_ = nullptr;
    VideoCallback* cb_ = nullptr;
    SIZE native_{0, 0};
};

bool RegisterMediaClasses(HINSTANCE inst);
// Tworzy okno-gospodarza dla MediaView (klasa "KXMedia").
HWND CreateMediaHost(HWND parent, DWORD style, DWORD exStyle, int x, int y, int w, int h, MediaView* view);

// Tapeta: po jednym oknie na monitor, podpietym pod pulpit (za ikonami).
class Wallpaper {
public:
    bool Start(const std::wstring& path, bool mute, int fit, std::wstring& err);
    void Stop();
    bool Running() const { return !surfaces_.empty(); }
    const std::wstring& Path() const { return path_; }
    void SetPaused(bool paused);
    void SetMute(bool mute);
    void SetFit(int fit);

private:
    struct Surface {
        HWND hwnd = nullptr;
        std::unique_ptr<MediaView> view;
    };
    std::vector<Surface> surfaces_;
    std::wstring path_;
    bool mute_ = true;
    int fit_ = kFill;
    bool paused_ = false;
};
