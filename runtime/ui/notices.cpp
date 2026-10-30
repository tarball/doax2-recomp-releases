#include "ui/notices.h"

#include "log.h"
#include "util/image.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>

#include <windows.h>

namespace ui::notices
{
    namespace
    {
        // The card's layout in a 1280x720 frame: at the bottom of the title-safe area (5% in), centered, where the
        // console's own notices appeared.
        constexpr float kFrameWidth = 1280, kFrameHeight = 720;
        constexpr float kHeight = 84, kBottom = 36, kRadius = 12, kPadding = 10, kPicture = 64;
        constexpr float kTextLeftAfterPicture = kPadding + kPicture + 14, kTextLeft = 24, kTextRight = 22, kDetailGap = 24;
        constexpr float kHeadingSize = 17, kTextSize = 23;
        constexpr float kMinWidth = 360, kMaxWidth = 760;
        constexpr float kOpacity = 0.92f;
        constexpr uint32_t kBackground = 0x00221E1C; // BGRX
        constexpr uint32_t kFadeIn = 12, kFadeOut = 24;
        constexpr size_t kMaxQueued = 16;

        std::mutex g_lock;
        std::deque<Notice> g_queue;

        // The notice shown now (the renderer's thread's).
        struct Showing
        {
            bool active = false;
            Notice notice;
            uint32_t age = 0;
            bool drawn = false;
            std::shared_ptr<const Card> card; // (nullptr if it couldn't be drawn)
            uint32_t cardHeight = 0; // the frame height its card was drawn for
        };
        Showing g_showing;

        std::wstring Wide(const std::string& text)
        {
            if (text.empty())
                return {};
            const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
            std::wstring wide(size_t(std::max(length, 0)), L'\0');
            if (length > 0)
                MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(), length);
            return wide;
        }

        HFONT Font(float pixels, int weight)
        {
            return CreateFontW(-int(std::lround(pixels)), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        }

        float TextWidth(HDC dc, HFONT font, const std::wstring& text)
        {
            SelectObject(dc, font);
            SIZE size{};
            GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
            return float(size.cx);
        }

        // How much of pixel (x, y) the card's rounded rectangle covers, w x h pixels with corners of radius r (its
        // signed distance from the pixel's center, over a pixel's width).
        float Coverage(uint32_t x, uint32_t y, float w, float h, float r)
        {
            const float qx = std::fabs(float(x) + 0.5f - w * 0.5f) - (w * 0.5f - r);
            const float qy = std::fabs(float(y) + 0.5f - h * 0.5f) - (h * 0.5f - r);
            const float distance = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
            return std::clamp(0.5f - distance, 0.0f, 1.0f);
        }

        // The notice's card, drawn `scale` times its size in a 1280x720 frame.
        std::shared_ptr<const Card> Draw(const Notice& notice, float scale)
        {
            const std::wstring heading = Wide(notice.heading), text = Wide(notice.text), detail = Wide(notice.detail);
            const HDC dc = CreateCompatibleDC(nullptr);
            if (!dc)
                return nullptr;
            const HFONT headingFont = Font(kHeadingSize * scale, FW_NORMAL), detailFont = Font(kHeadingSize * scale, FW_SEMIBOLD),
                        textFont = Font(kTextSize * scale, FW_SEMIBOLD);
            const bool picture = !notice.picture.empty();
            const float left = picture ? kTextLeftAfterPicture : kTextLeft;
            const float headingWidth = TextWidth(dc, headingFont, heading) / scale, textWidth = TextWidth(dc, textFont, text) / scale,
                        detailWidth = detail.empty() ? 0.0f : TextWidth(dc, detailFont, detail) / scale + kDetailGap;
            const float width = std::clamp(left + std::max(headingWidth + detailWidth, textWidth) + kTextRight, kMinWidth, kMaxWidth);

            auto card = std::make_shared<Card>();
            card->w = width;
            card->h = kHeight;
            card->x = (kFrameWidth - width) * 0.5f;
            card->y = kFrameHeight - kBottom - kHeight;
            card->width = uint32_t(std::lround(width * scale));
            card->height = uint32_t(std::lround(kHeight * scale));
            const uint32_t w = card->width, h = card->height;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = LONG(w);
            info.bmiHeader.biHeight = -LONG(h); // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap && headingFont && detailFont && textFont)
            {
                const HGDIOBJ previous = SelectObject(dc, bitmap);
                uint32_t* pixels = static_cast<uint32_t*>(bits);
                std::fill(pixels, pixels + size_t(w) * h, kBackground);
                // The picture, over the background.
                if (picture)
                {
                    uint32_t size = uint32_t(std::lround(kPicture * scale)), pictureWidth = size, pictureHeight = size;
                    std::vector<uint8_t> bgra;
                    if (util::DecodeImage(notice.picture.data(), notice.picture.size(), pictureWidth, pictureHeight, true, bgra))
                    {
                        const uint32_t x0 = uint32_t(std::lround(kPadding * scale)), y0 = uint32_t(std::lround((kHeight - kPicture) * 0.5f * scale));
                        for (uint32_t y = 0; y < pictureHeight && y0 + y < h; y++)
                        {
                            for (uint32_t x = 0; x < pictureWidth && x0 + x < w; x++)
                            {
                                const uint8_t* source = &bgra[(size_t(y) * pictureWidth + x) * 4];
                                uint8_t* target = reinterpret_cast<uint8_t*>(&pixels[size_t(y0 + y) * w + x0 + x]);
                                for (int c = 0; c < 3; c++)
                                    target[c] = uint8_t(source[c] + (target[c] * (255 - source[3]) + 127) / 255);
                            }
                        }
                    }
                }
                // The text: the heading, the detail right of it, and the text below.
                const auto rect = [scale](float x0, float y0, float x1, float y1) {
                    return RECT{ LONG(std::lround(x0 * scale)), LONG(std::lround(y0 * scale)), LONG(std::lround(x1 * scale)), LONG(std::lround(y1 * scale)) };
                };
                SetBkMode(dc, TRANSPARENT);
                constexpr UINT kLine = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
                RECT headingRect = rect(left, 12, width - kTextRight, 38);
                SelectObject(dc, detailFont);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, detail.c_str(), -1, &headingRect, kLine | DT_RIGHT);
                headingRect.right -= LONG(std::lround(detailWidth * scale));
                SelectObject(dc, headingFont);
                SetTextColor(dc, RGB(196, 196, 202));
                DrawTextW(dc, heading.c_str(), -1, &headingRect, kLine | DT_LEFT);
                RECT textRect = rect(left, 38, width - kTextRight, 72);
                SelectObject(dc, textFont);
                SetTextColor(dc, RGB(255, 255, 255));
                DrawTextW(dc, text.c_str(), -1, &textRect, kLine | DT_LEFT);
                GdiFlush();
                // The card's rounded shape, and premultiplied alpha.
                card->bgra.resize(size_t(w) * h * 4);
                const float radius = kRadius * scale;
                for (uint32_t y = 0; y < h; y++)
                {
                    for (uint32_t x = 0; x < w; x++)
                    {
                        const float alpha = Coverage(x, y, float(w), float(h), radius) * kOpacity;
                        const uint8_t* source = reinterpret_cast<const uint8_t*>(&pixels[size_t(y) * w + x]);
                        uint8_t* target = &card->bgra[(size_t(y) * w + x) * 4];
                        for (int c = 0; c < 3; c++)
                            target[c] = uint8_t(std::lround(source[c] * alpha));
                        target[3] = uint8_t(std::lround(255.0f * alpha));
                    }
                }
                SelectObject(dc, previous);
            }
            for (const HGDIOBJ object : { HGDIOBJ(bitmap), HGDIOBJ(headingFont), HGDIOBJ(detailFont), HGDIOBJ(textFont) })
            {
                if (object)
                    DeleteObject(object);
            }
            DeleteDC(dc);
            if (card->bgra.empty())
            {
                LOG_WARN("Notices: can't draw a notice's card");
                return nullptr;
            }
            return card;
        }
    }

    void Show(Notice notice)
    {
        std::lock_guard lock(g_lock);
        if (g_queue.size() < kMaxQueued)
            g_queue.push_back(std::move(notice));
    }

    Frame Next(uint32_t pixelHeight)
    {
        Showing& s = g_showing;
        if (!s.active)
        {
            std::lock_guard lock(g_lock);
            if (g_queue.empty())
                return {};
            s = Showing{};
            s.active = true;
            s.notice = std::move(g_queue.front());
            g_queue.pop_front();
        }
        // Drawn again if the frame is now shown larger (the window enlarged, the resolution scale raised).
        const uint32_t height = std::clamp<uint32_t>(pixelHeight, 180, 8640);
        if (!s.drawn || height > s.cardHeight + s.cardHeight / 10)
        {
            s.card = Draw(s.notice, float(height) / kFrameHeight);
            s.cardHeight = height;
            s.drawn = true;
        }
        const uint32_t frames = std::max(s.notice.frames, kFadeIn + kFadeOut);
        Frame frame;
        frame.card = s.card;
        frame.opacity = std::min(1.0f, float(s.age + 1) / kFadeIn) * std::min(1.0f, float(frames - s.age) / kFadeOut);
        if (++s.age >= frames)
            s = Showing{};
        return frame;
    }
}
