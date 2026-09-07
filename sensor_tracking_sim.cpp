#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

// This program is intentionally written in an embedded style:
// - no dynamic memory allocation
// - fixed-size circular buffer
// - deterministic packet parsing and state transitions
// - explicit-width integer types for portability and memory control

namespace telemetry
{
    constexpr const char* kUsgsFeedUrl = "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/all_hour.geojson";
    constexpr const char* kIssFeedUrl = "https://api.wheretheiss.at/v1/satellites/25544";
    constexpr const char* kSatelliteLoopPageUrl = "https://www.star.nesdis.noaa.gov/GOES/conus.php?sat=G19";

    constexpr std::uint16_t kStatusIdle       = 0x0u;
    constexpr std::uint16_t kStatusAcquiring  = 0x1u;
    constexpr std::uint16_t kStatusTracking   = 0x2u;
    constexpr std::uint16_t kStatusFault      = 0x3u;

    constexpr std::uint16_t kWord0ValidMask   = 0x8000u;
    constexpr std::uint16_t kWord0StatusMask   = 0x7000u;
    constexpr std::uint16_t kWord0TargetMask   = 0x0F80u;
    constexpr std::uint16_t kWord0SequenceMask = 0x007Fu;

    constexpr std::uint16_t kWord1PitchMask    = 0xFC00u;
    constexpr std::uint16_t kWord1YawMask      = 0x03F0u;
    constexpr std::uint16_t kWord1QualityMask  = 0x000Fu;

    constexpr std::uint16_t kWord2DistanceMask = 0xFFC0u;
    constexpr std::uint16_t kWord2ChecksumMask = 0x003Fu;

    constexpr std::uint16_t kPitchOffset = 32u;
    constexpr std::uint16_t kYawOffset   = 32u;

    constexpr std::size_t kFrameWords = 3u;
    constexpr std::size_t kQueueDepth  = 8u;

    struct RawFrame
    {
        std::array<std::uint16_t, kFrameWords> words{};
    };

    struct Target
    {
        std::uint8_t  id = 0u;
        std::int16_t  pitchDeg = 0;
        std::int16_t  yawDeg = 0;
        std::uint16_t distanceMeters = 0u;
        std::uint8_t  quality = 0u;
        bool          valid = false;
    };

    enum class TrackState : std::uint8_t
    {
        IDLE = 0u,
        ACQUIRING,
        TRACKING,
        FAULT
    };

    struct DecodedFrame
    {
        bool checksumValid = false;
        std::uint8_t status = 0u;
        std::uint8_t sequence = 0u;
        Target target{};
    };

    struct UsgsObservation
    {
        double magnitude = 0.0;
        double longitude = 0.0;
        double latitude = 0.0;
        double depthKm = 0.0;
        std::array<char, 96> place{};
    };

    struct IssObservation
    {
        double latitude = 0.0;
        double longitude = 0.0;
        double altitudeKm = 0.0;
        double velocityKph = 0.0;
        std::array<char, 16> visibility{};
    };

    struct SatelliteObservation
    {
        std::array<char, 256> loopGifUrl{};
        std::array<char, 128> productName{};
        std::array<char, 64> updatedText{};
    };

    struct SourceStats
    {
        std::array<std::array<char, 32>, 4u> metricLabels{};
        std::array<std::uint32_t, 4u> metricValues{};
        std::array<char, 256> summary{};
    };

    #if 0
    enum class LiveSourceMode : std::uint8_t
    {
        Synthetic = 0u,
        Usgs,
        Iss,
        Satellite
    };

    struct GuiAppState
    {
        SensorManager manager{};
        LiveSourceMode source = LiveSourceMode::Synthetic;
        std::uint32_t tick = 0u;
        SourceStats currentStats{};
        static constexpr std::size_t kGraphSamples = 240u;
        std::array<std::array<std::uint32_t, kGraphSamples>, 4u> metricHistory{};
        std::size_t historyCount = 0u;
        std::size_t historyHead = 0u;
        std::array<char, 16384> logBuffer{};
        std::size_t logLength = 0u;
        HWND hwndState = nullptr;
        HWND hwndFrames = nullptr;
        HWND hwndMissed = nullptr;
        HWND hwndFaults = nullptr;
        HWND hwndOverflows = nullptr;
        HWND hwndTarget = nullptr;
        HWND hwndLog = nullptr;
        HWND hwndSourceCombo = nullptr;
    };

#ifdef _WIN32
    static bool FetchUsgsAllHourGeoJson(std::array<char, 16384>& buffer, std::size_t& bytesWritten);
    static bool FetchIssCurrentPosition(std::array<char, 4096>& buffer, std::size_t& bytesWritten);
#endif

    static const char* ToString(LiveSourceMode source)
    {
        switch (source)
        {
            case LiveSourceMode::Synthetic: return "Synthetic";
            case LiveSourceMode::Usgs:      return "USGS";
            case LiveSourceMode::Iss:       return "ISS";
            case LiveSourceMode::Satellite: return "Satellite Loop";
        }

        return "Unknown";
    }

    static void ClearLog(GuiAppState& app)
    {
        app.logBuffer.fill('\0');
        app.logLength = 0u;
    }

    static void AppendLogLine(GuiAppState& app, const char* text)
    {
        if (text == nullptr)
        {
            return;
        }

        const std::size_t textLength = std::strlen(text);
        const std::size_t required = textLength + 2u;
        if (required >= app.logBuffer.size())
        {
            return;
        }

        if (app.logLength + required >= app.logBuffer.size())
        {
            ClearLog(app);
        }

        std::memcpy(app.logBuffer.data() + app.logLength, text, textLength);
        app.logLength += textLength;
        app.logBuffer[app.logLength++] = '\r';
        app.logBuffer[app.logLength++] = '\n';
        app.logBuffer[app.logLength] = '\0';
    }

    static void ResetGraphHistory(GuiAppState& app)
    {
        app.historyCount = 0u;
        app.historyHead = 0u;
        for (auto& series : app.metricHistory)
        {
            series.fill(0u);
        }
    }

    static void ScrollLogToBottom(HWND hwndLog)
    {
        if (hwndLog == nullptr)
        {
            return;
        }

        const LRESULT textLength = SendMessageA(hwndLog, WM_GETTEXTLENGTH, 0, 0);
        SendMessageA(hwndLog, EM_SETSEL, static_cast<WPARAM>(textLength), static_cast<LPARAM>(textLength));
        SendMessageA(hwndLog, EM_SCROLLCARET, 0, 0);
    }

    static void PushHistorySample(GuiAppState& app)
    {
        for (std::size_t seriesIndex = 0u; seriesIndex < app.currentStats.metricValues.size(); ++seriesIndex)
        {
            app.metricHistory[seriesIndex][app.historyHead] = app.currentStats.metricValues[seriesIndex];
        }

        app.historyHead = (app.historyHead + 1u) % GuiAppState::kGraphSamples;
        if (app.historyCount < GuiAppState::kGraphSamples)
        {
            ++app.historyCount;
        }
    }

    static void GetHistoryBounds(const GuiAppState& app,
                                 const std::array<std::uint32_t, GuiAppState::kGraphSamples>& values,
                                 std::uint32_t& minimum,
                                 std::uint32_t& maximum)
    {
        minimum = 0u;
        maximum = 1u;
        if (app.historyCount == 0u)
        {
            return;
        }

        bool initialized = false;
        for (std::size_t index = 0u; index < app.historyCount; ++index)
        {
            const std::size_t sampleIndex = (app.historyHead + GuiAppState::kGraphSamples - app.historyCount + index) % GuiAppState::kGraphSamples;
            const std::uint32_t sample = values[sampleIndex];
            if (!initialized)
            {
                minimum = sample;
                maximum = sample;
                initialized = true;
                continue;
            }

            if (sample < minimum)
            {
                minimum = sample;
            }
            if (sample > maximum)
            {
                maximum = sample;
            }
        }

        if (minimum == maximum)
        {
            if (minimum == 0u)
            {
                maximum = 1u;
            }
            else
            {
                minimum -= 1u;
                maximum += 1u;
            }
        }
    }

    static void DrawGraphSeries(HDC deviceContext,
                                const RECT& bounds,
                                const GuiAppState& app,
                                const std::array<std::uint32_t, GuiAppState::kGraphSamples>& values,
                                COLORREF lineColor,
                                COLORREF fillColor,
                                const char* title)
    {
        const HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
        FillRect(deviceContext, &bounds, background);
        DeleteObject(background);

        const HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
        const HPEN linePen = CreatePen(PS_SOLID, 2, lineColor);
        const HPEN fillPen = CreatePen(PS_SOLID, 1, fillColor);
        const HBRUSH noBrush = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        const HGDIOBJ oldBrush = SelectObject(deviceContext, noBrush);
        const HGDIOBJ oldBorder = SelectObject(deviceContext, borderPen);

        Rectangle(deviceContext, bounds.left, bounds.top, bounds.right, bounds.bottom);

        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, RGB(30, 30, 30));
        TextOutA(deviceContext, bounds.left + 6, bounds.top + 4, title, static_cast<int>(std::strlen(title)));

        const int plotLeft = bounds.left + 8;
        const int plotTop = bounds.top + 24;
        const int plotRight = bounds.right - 8;
        const int plotBottom = bounds.bottom - 10;
        const int plotWidth = plotRight - plotLeft;
        const int plotHeight = plotBottom - plotTop;

        MoveToEx(deviceContext, plotLeft, plotBottom, nullptr);
        LineTo(deviceContext, plotRight, plotBottom);
        MoveToEx(deviceContext, plotLeft, plotTop, nullptr);
        LineTo(deviceContext, plotLeft, plotBottom);

        std::uint32_t minimum = 0u;
        std::uint32_t maximum = 1u;
        GetHistoryBounds(app, values, minimum, maximum);
        const std::uint32_t range = (maximum > minimum) ? (maximum - minimum) : 1u;
        if (app.historyCount >= 2u)
        {
            SelectObject(deviceContext, linePen);
            const std::size_t startIndex = (app.historyHead + GuiAppState::kGraphSamples - app.historyCount) % GuiAppState::kGraphSamples;
            for (std::size_t index = 0u; index < app.historyCount; ++index)
            {
                const std::size_t sampleIndex = (startIndex + index) % GuiAppState::kGraphSamples;
                const int x = plotLeft + static_cast<int>((index * plotWidth) / (app.historyCount - 1u));
                const std::uint32_t sample = values[sampleIndex];
                const int y = plotBottom - static_cast<int>(((sample - minimum) * plotHeight) / range);
                if (index == 0u)
                {
                    MoveToEx(deviceContext, x, y, nullptr);
                }
                else
                {
                    LineTo(deviceContext, x, y);
                }
            }
        }

        SelectObject(deviceContext, fillPen);
        MoveToEx(deviceContext, plotLeft, plotBottom, nullptr);

        SelectObject(deviceContext, oldBorder);
        SelectObject(deviceContext, oldBrush);
        DeleteObject(borderPen);
        DeleteObject(linePen);
        DeleteObject(fillPen);
    }

    static void PaintGraphs(HWND hwnd, HDC deviceContext, const GuiAppState& app)
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);

        const int graphTop = 140;
        const int graphMargin = 12;
        const int graphHeight = 160;
        const int graphWidth = (clientRect.right - (graphMargin * 3)) / 2;

        RECT framesRect{graphMargin, graphTop, graphMargin + graphWidth, graphTop + graphHeight};
        RECT faultsRect{graphMargin * 2 + graphWidth, graphTop, graphMargin * 2 + graphWidth * 2, graphTop + graphHeight};
        RECT missedRect{graphMargin, graphTop + graphHeight + graphMargin, graphMargin + graphWidth, graphTop + graphHeight * 2 + graphMargin};
        RECT rangeRect{graphMargin * 2 + graphWidth, graphTop + graphHeight + graphMargin, graphMargin * 2 + graphWidth * 2, graphTop + graphHeight * 2 + graphMargin};

        DrawGraphSeries(deviceContext, framesRect, app, app.framesHistory, RGB(0, 102, 204), RGB(220, 235, 255), "Frames Processed");
        DrawGraphSeries(deviceContext, faultsRect, app, app.faultsHistory, RGB(204, 0, 0), RGB(255, 230, 230), "Checksum Faults");
        DrawGraphSeries(deviceContext, missedRect, app, app.missedHistory, RGB(240, 140, 0), RGB(255, 242, 224), "Missed Ticks");
        DrawGraphSeries(deviceContext, rangeRect, app, app.rangeHistory, RGB(0, 150, 90), RGB(225, 250, 236), "Target Range");
    }

    static void PushHistorySample(GuiAppState& app)
    {
        app.framesHistory[app.historyHead] = app.manager.processedFrames();
        app.faultsHistory[app.historyHead] = app.manager.checksumFaults();
        app.missedHistory[app.historyHead] = app.manager.ticksSinceLastFrame();
        app.rangeHistory[app.historyHead] = app.manager.target().distanceMeters;
        app.historyHead = (app.historyHead + 1u) % GuiAppState::kGraphSamples;
        if (app.historyCount < GuiAppState::kGraphSamples)
        {
            ++app.historyCount;
        }
    }

    static void GetHistoryBounds(const GuiAppState& app,
                                 const std::array<std::uint32_t, GuiAppState::kGraphSamples>& values,
                                 std::uint32_t& minimum,
                                 std::uint32_t& maximum)
    {
        minimum = 0u;
        maximum = 1u;
        if (app.historyCount == 0u)
        {
            return;
        }

        bool initialized = false;
        for (std::size_t index = 0u; index < app.historyCount; ++index)
        {
            const std::size_t sampleIndex = (app.historyHead + GuiAppState::kGraphSamples - app.historyCount + index) % GuiAppState::kGraphSamples;
            const std::uint32_t sample = values[sampleIndex];
            if (!initialized)
            {
                minimum = sample;
                maximum = sample;
                initialized = true;
                continue;
            }

            if (sample < minimum)
            {
                minimum = sample;
            }
            if (sample > maximum)
            {
                maximum = sample;
            }
        }

        if (minimum == maximum)
        {
            if (minimum == 0u)
            {
                maximum = 1u;
            }
            else
            {
                minimum -= 1u;
                maximum += 1u;
            }
        }
    }

    static std::uint32_t SourceZoomMultiplier(LiveSourceMode source)
    {
        switch (source)
        {
            case LiveSourceMode::Iss:
                return 8u;
            case LiveSourceMode::Usgs:
                return 4u;
            case LiveSourceMode::Satellite:
                return 1u;
            case LiveSourceMode::Synthetic:
            default:
                return 2u;
        }
    }

    static void DrawGraphSeries(HDC deviceContext,
                                const RECT& bounds,
                                const GuiAppState& app,
                                const std::array<std::uint32_t, GuiAppState::kGraphSamples>& values,
                                COLORREF lineColor,
                                COLORREF fillColor,
                                const char* title,
                                std::uint32_t zoomMultiplier)
    {
        const HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
        FillRect(deviceContext, &bounds, background);
        DeleteObject(background);

        const HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
        const HPEN linePen = CreatePen(PS_SOLID, 2, lineColor);
        const HPEN fillPen = CreatePen(PS_SOLID, 1, fillColor);
        const HBRUSH noBrush = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        const HGDIOBJ oldBrush = SelectObject(deviceContext, noBrush);
        const HGDIOBJ oldBorder = SelectObject(deviceContext, borderPen);

        Rectangle(deviceContext, bounds.left, bounds.top, bounds.right, bounds.bottom);

        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, RGB(30, 30, 30));
        TextOutA(deviceContext, bounds.left + 6, bounds.top + 4, title, static_cast<int>(std::strlen(title)));

        const int plotLeft = bounds.left + 8;
        const int plotTop = bounds.top + 24;
        const int plotRight = bounds.right - 8;
        const int plotBottom = bounds.bottom - 10;
        const int plotWidth = plotRight - plotLeft;
        const int plotHeight = plotBottom - plotTop;

        MoveToEx(deviceContext, plotLeft, plotBottom, nullptr);
        LineTo(deviceContext, plotRight, plotBottom);
        MoveToEx(deviceContext, plotLeft, plotTop, nullptr);
        LineTo(deviceContext, plotLeft, plotBottom);

        std::uint32_t minimum = 0u;
        std::uint32_t maximum = 1u;
        GetHistoryBounds(app, values, minimum, maximum);
        const std::uint32_t span = (maximum > minimum) ? (maximum - minimum) : 1u;
        const std::uint32_t displaySpan = std::max(1u, span / std::max(1u, zoomMultiplier));
        if (app.historyCount >= 2u)
        {
            SelectObject(deviceContext, linePen);
            const std::size_t startIndex = (app.historyHead + GuiAppState::kGraphSamples - app.historyCount) % GuiAppState::kGraphSamples;
            for (std::size_t index = 0u; index < app.historyCount; ++index)
            {
                const std::size_t sampleIndex = (startIndex + index) % GuiAppState::kGraphSamples;
                const int x = plotLeft + static_cast<int>((index * plotWidth) / (app.historyCount - 1u));
                const std::uint32_t sample = values[sampleIndex];
                const std::uint32_t adjusted = (sample >= minimum) ? (sample - minimum) : 0u;
                const int y = plotBottom - static_cast<int>((adjusted * plotHeight) / displaySpan);
                if (index == 0u)
                {
                    MoveToEx(deviceContext, x, y, nullptr);
                }
                else
                {
                    LineTo(deviceContext, x, y);
                }
            }
        }

        SelectObject(deviceContext, fillPen);
        MoveToEx(deviceContext, plotLeft, plotBottom, nullptr);

        SelectObject(deviceContext, oldBorder);
        SelectObject(deviceContext, oldBrush);
        DeleteObject(borderPen);
        DeleteObject(linePen);
        DeleteObject(fillPen);
    }

    static void PaintGraphs(HWND hwnd, HDC deviceContext, const GuiAppState& app)
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);

        const int graphTop = 140;
        const int graphMargin = 12;
        const int graphHeight = 160;
        const int graphWidth = (clientRect.right - (graphMargin * 3)) / 2;

        RECT framesRect{graphMargin, graphTop, graphMargin + graphWidth, graphTop + graphHeight};
        RECT faultsRect{graphMargin * 2 + graphWidth, graphTop, graphMargin * 2 + graphWidth * 2, graphTop + graphHeight};
        RECT missedRect{graphMargin, graphTop + graphHeight + graphMargin, graphMargin + graphWidth, graphTop + graphHeight * 2 + graphMargin};
        RECT rangeRect{graphMargin * 2 + graphWidth, graphTop + graphHeight + graphMargin, graphMargin * 2 + graphWidth * 2, graphTop + graphHeight * 2 + graphMargin};

        const std::uint32_t zoomMultiplier = SourceZoomMultiplier(app.source);
        DrawGraphSeries(deviceContext, framesRect, app, app.metricHistory[0], RGB(0, 102, 204), RGB(220, 235, 255), app.currentStats.metricLabels[0].data(), zoomMultiplier);
        DrawGraphSeries(deviceContext, faultsRect, app, app.metricHistory[1], RGB(204, 0, 0), RGB(255, 230, 230), app.currentStats.metricLabels[1].data(), zoomMultiplier);
        DrawGraphSeries(deviceContext, missedRect, app, app.metricHistory[2], RGB(240, 140, 0), RGB(255, 242, 224), app.currentStats.metricLabels[2].data(), zoomMultiplier);
        DrawGraphSeries(deviceContext, rangeRect, app, app.metricHistory[3], RGB(0, 150, 90), RGB(225, 250, 236), app.currentStats.metricLabels[3].data(), zoomMultiplier);
    }

    static UINT TimerIntervalForSource(LiveSourceMode source)
    {
        switch (source)
        {
            case LiveSourceMode::Satellite:
                return 5000u;
            case LiveSourceMode::Iss:
            case LiveSourceMode::Usgs:
            case LiveSourceMode::Synthetic:
            default:
                return 1000u;
        }
    }

    static void FormatStateLine(const GuiAppState& app, char* output, std::size_t outputSize)
    {
        if (output == nullptr || outputSize == 0u)
        {
            return;
        }

        std::snprintf(output,
                      outputSize,
                      "State=%s | Source=%s | Tick=%u | %s",
                      ToString(app.manager.state()),
                      ToString(app.source),
                      static_cast<unsigned>(app.tick),
                      app.currentStats.summary.data());
    }

    static bool BuildSyntheticFrame(std::uint32_t tick, RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
        const bool corruptChecksum = (tick % 11u) == 7u;
        const std::uint8_t status = (tick == 0u) ? kStatusIdle : ((tick < 3u) ? kStatusAcquiring : kStatusTracking);
        const std::uint16_t distance = static_cast<std::uint16_t>(100u + (tick % 20u));
        const std::int16_t pitch = static_cast<std::int16_t>((tick % 13u) - 6);
        const std::int16_t yaw = static_cast<std::int16_t>(((tick * 2u) % 13u) - 6);

        frame = SensorManager::PackFrame(status,
                                         1u,
                                         pitch,
                                         yaw,
                                         distance,
                                         static_cast<std::uint8_t>(tick & 0x7Fu),
                                         4u,
                                         corruptChecksum);

                        std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Status");
                        std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Pitch+32");
                        std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Yaw+32");
                        std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "Range m");
                        stats.metricValues[0] = status;
                        stats.metricValues[1] = static_cast<std::uint32_t>(pitch + 32);
                        stats.metricValues[2] = static_cast<std::uint32_t>(yaw + 32);
                        stats.metricValues[3] = distance;
                        std::snprintf(stats.summary.data(), stats.summary.size(),
                                  "Synthetic packet | status=%u pitch=%d yaw=%d range=%u checksum=%s",
                                  static_cast<unsigned>(status),
                                  static_cast<int>(pitch),
                                  static_cast<int>(yaw),
                                  static_cast<unsigned>(distance),
                                  corruptChecksum ? "bad" : "ok");

        std::snprintf(info,
                      infoSize,
                      "Synthetic tick %u | status=%u | pitch=%d | yaw=%d | range=%u | checksum=%s",
                      static_cast<unsigned>(tick),
                      static_cast<unsigned>(status),
                      static_cast<int>(pitch),
                      static_cast<int>(yaw),
                      static_cast<unsigned>(distance),
                      corruptChecksum ? "bad" : "ok");
        return true;
    }

    static bool BuildUsgsFrame(RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
#ifdef _WIN32
        std::array<char, 16384> jsonBuffer{};
        std::size_t jsonBytes = 0u;
        UsgsObservation observation{};

        if (!FetchUsgsAllHourGeoJson(jsonBuffer, jsonBytes) || !ParseUsgsObservation(jsonBuffer.data(), observation))
        {
            std::snprintf(info, infoSize, "USGS fetch failed");
            return false;
        }

        frame = BuildFrameFromUsgsObservation(observation);
        std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Mag x100");
        std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Lat+90 x100");
        std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Lon+180 x100");
        std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "Depth x10");
        stats.metricValues[0] = static_cast<std::uint32_t>(observation.magnitude * 100.0);
        stats.metricValues[1] = static_cast<std::uint32_t>((observation.latitude + 90.0) * 100.0);
        stats.metricValues[2] = static_cast<std::uint32_t>((observation.longitude + 180.0) * 100.0);
        stats.metricValues[3] = static_cast<std::uint32_t>(observation.depthKm * 10.0);
        std::snprintf(stats.summary.data(), stats.summary.size(),
                  "USGS quake | M%.1f | %s | lat=%.3f lon=%.3f depth=%.1f km",
                  observation.magnitude,
                  observation.place.data(),
                  observation.latitude,
                  observation.longitude,
                  observation.depthKm);
        std::snprintf(info,
                      infoSize,
                      "USGS M%.1f | %s | lat=%.3f lon=%.3f depth=%.1f km",
                      observation.magnitude,
                      observation.place.data(),
                      observation.latitude,
                      observation.longitude,
                      observation.depthKm);
        return true;
#else
        (void)frame;
        std::snprintf(info, infoSize, "USGS live fetch is only available on Windows");
        return false;
#endif
    }

    static bool BuildIssFrame(RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
#ifdef _WIN32
        std::array<char, 4096> jsonBuffer{};
        std::size_t jsonBytes = 0u;
        IssObservation observation{};

        if (!FetchIssCurrentPosition(jsonBuffer, jsonBytes) || !ParseIssObservation(jsonBuffer.data(), observation))
        {
            std::snprintf(info, infoSize, "ISS fetch failed");
            return false;
        }

        frame = BuildFrameFromIssObservation(observation);
        std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Lat+90 x100");
        std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Lon+180 x100");
        std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Alt x100");
        std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "Vel kph");
        stats.metricValues[0] = static_cast<std::uint32_t>((observation.latitude + 90.0) * 100.0);
        stats.metricValues[1] = static_cast<std::uint32_t>((observation.longitude + 180.0) * 100.0);
        stats.metricValues[2] = static_cast<std::uint32_t>(observation.altitudeKm * 100.0);
        stats.metricValues[3] = static_cast<std::uint32_t>(observation.velocityKph);
        std::snprintf(stats.summary.data(), stats.summary.size(),
                  "ISS | lat=%.3f lon=%.3f alt=%.1f km vel=%.1f kph vis=%s",
                  observation.latitude,
                  observation.longitude,
                  observation.altitudeKm,
                  observation.velocityKph,
                  observation.visibility.data());
        std::snprintf(info,
                      infoSize,
                      "ISS lat=%.3f lon=%.3f alt=%.1f km vel=%.1f kph vis=%s",
                      observation.latitude,
                      observation.longitude,
                      observation.altitudeKm,
                      observation.velocityKph,
                      observation.visibility.data());
        return true;
#else
        (void)frame;
        std::snprintf(info, infoSize, "ISS live fetch is only available on Windows");
        return false;
#endif
    }

    static bool BuildSatelliteFrame(RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
#ifdef _WIN32
        std::array<char, 32768> htmlBuffer{};
        std::size_t bytesWritten = 0u;
        SatelliteObservation observation{};

        HINTERNET session = WinHttpOpen(L"TelemetrySim/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS,
                                        0);
        if (session == nullptr)
        {
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        HINTERNET connection = WinHttpConnect(session, L"www.star.nesdis.noaa.gov", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connection == nullptr)
        {
            WinHttpCloseHandle(session);
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(connection,
                                               L"GET",
                                               L"/GOES/conus.php?sat=G19",
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request == nullptr)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        DWORD availableBytes = 0u;
        while (WinHttpQueryDataAvailable(request, &availableBytes) && availableBytes > 0u)
        {
            const std::size_t remaining = htmlBuffer.size() - bytesWritten - 1u;
            if (remaining == 0u)
            {
                break;
            }

            const DWORD bytesToRead = static_cast<DWORD>((availableBytes < remaining) ? availableBytes : remaining);
            DWORD bytesRead = 0u;
            if (!WinHttpReadData(request, htmlBuffer.data() + bytesWritten, bytesToRead, &bytesRead))
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                std::snprintf(info, infoSize, "Satellite page fetch failed");
                return false;
            }

            bytesWritten += bytesRead;
            if (bytesWritten >= htmlBuffer.size() - 1u)
            {
                break;
            }
        }

        htmlBuffer[bytesWritten] = '\0';
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);

        if (!ParseSatelliteObservation(htmlBuffer.data(), observation))
        {
            std::snprintf(info, infoSize, "Satellite loop parse failed");
            return false;
        }

        const std::uint32_t tick = static_cast<std::uint32_t>(std::strtoul(observation.loopGifUrl.data(), nullptr, 10) & 0x7Fu);
        frame = SensorManager::PackFrame(kStatusTracking,
                                         1u,
                                         static_cast<std::int16_t>((tick % 13u) - 6u),
                                         static_cast<std::int16_t>(((tick * 3u) % 13u) - 6u),
                                         150u,
                                         static_cast<std::uint8_t>(tick),
                                         5u,
                                         false);

        std::snprintf(info,
                      infoSize,
                      "Satellite loop | %s | %s",
                      observation.productName.data(),
                      kSatelliteLoopPageUrl);
        return true;
#else
        (void)frame;
        std::snprintf(info, infoSize, "Satellite loop is only available on Windows in this build");
        return false;
#endif
    }

    static bool BuildFrameForSource(LiveSourceMode source, std::uint32_t tick, RawFrame& frame, char* info, std::size_t infoSize)
    {
        switch (source)
        {
            case LiveSourceMode::Synthetic:
                return BuildSyntheticFrame(tick, frame, info, infoSize);
            case LiveSourceMode::Usgs:
                return BuildUsgsFrame(frame, info, infoSize);
            case LiveSourceMode::Iss:
                return BuildIssFrame(frame, info, infoSize);
            case LiveSourceMode::Satellite:
                return BuildSatelliteFrame(frame, info, infoSize);
        }

        std::snprintf(info, infoSize, "Unknown source");
        return false;
    }
    #endif

    template <typename T>
    constexpr T ClampValue(T value, T low, T high)
    {
        return (value < low) ? low : ((value > high) ? high : value);
    }

    static bool CopyQuotedJsonValue(const char* start, char* destination, std::size_t destinationSize)
    {
        if (start == nullptr || destination == nullptr || destinationSize == 0u)
        {
            return false;
        }

        const char* cursor = start;
        std::size_t index = 0u;
        while (*cursor != '\0' && *cursor != '"')
        {
            if (index + 1u >= destinationSize)
            {
                destination[0] = '\0';
                return false;
            }

            destination[index++] = *cursor++;
        }

        destination[index] = '\0';
        return index > 0u;
    }

    static bool ParseUsgsObservation(const char* json, UsgsObservation& observation)
    {
        if (json == nullptr)
        {
            return false;
        }

        const char* magnitudeKey = std::strstr(json, "\"mag\":");
        const char* placeKey = std::strstr(json, "\"place\":\"");
        const char* coordinatesKey = std::strstr(json, "\"coordinates\":[");

        if (magnitudeKey == nullptr || coordinatesKey == nullptr)
        {
            return false;
        }

        observation.magnitude = std::strtod(magnitudeKey + 6, nullptr);

        if (placeKey != nullptr)
        {
            CopyQuotedJsonValue(placeKey + 9, observation.place.data(), observation.place.size());
        }

        coordinatesKey += 15;
        char* endPtr = nullptr;
        observation.longitude = std::strtod(coordinatesKey, &endPtr);
        if (endPtr == nullptr || *endPtr != ',')
        {
            return false;
        }

        observation.latitude = std::strtod(endPtr + 1, &endPtr);
        if (endPtr == nullptr || *endPtr != ',')
        {
            return false;
        }

        observation.depthKm = std::strtod(endPtr + 1, nullptr);
        return true;
    }

    static RawFrame BuildFrameFromUsgsObservation(const UsgsObservation& observation)
    {
        const auto clampSignedField = [](double value) -> std::int16_t
        {
            const double limited = ClampValue(value, -32.0, 31.0);
            return static_cast<std::int16_t>(limited);
        };

        const auto clampUnsignedField = [](double value) -> std::uint16_t
        {
            const double limited = ClampValue(value, 1.0, 1023.0);
            return static_cast<std::uint16_t>(limited);
        };

        const std::int16_t pitchDeg = clampSignedField(observation.latitude / 2.0);
        const std::int16_t yawDeg = clampSignedField(observation.longitude / 5.625);
        const std::uint16_t rangeMeters = clampUnsignedField(observation.depthKm * 10.0);
        const std::uint8_t quality = static_cast<std::uint8_t>(ClampValue(observation.magnitude * 2.0, 0.0, 15.0));
        const std::uint8_t status = (observation.magnitude >= 4.5) ? kStatusTracking : kStatusAcquiring;

        RawFrame frame{};
        frame.words[0] = kWord0ValidMask;
        frame.words[0] |= static_cast<std::uint16_t>((static_cast<std::uint16_t>(status) & 0x7u) << 12u);
        frame.words[0] |= static_cast<std::uint16_t>((1u & 0x1Fu) << 7u);
        frame.words[0] |= 1u;

        const std::uint16_t packedPitch = static_cast<std::uint16_t>(static_cast<std::uint16_t>(pitchDeg + static_cast<std::int16_t>(kPitchOffset)) & 0x3Fu);
        const std::uint16_t packedYaw = static_cast<std::uint16_t>(static_cast<std::uint16_t>(yawDeg + static_cast<std::int16_t>(kYawOffset)) & 0x3Fu);
        frame.words[1] = static_cast<std::uint16_t>((packedPitch << 10u) | (packedYaw << 4u) | (quality & 0x0Fu));

        frame.words[2] = static_cast<std::uint16_t>((rangeMeters & 0x03FFu) << 6u);
        const std::uint16_t checksum = static_cast<std::uint16_t>((frame.words[0] ^ (frame.words[1] << 1u) ^ (frame.words[2] >> 1u)) & 0x003Fu);
        frame.words[2] |= checksum;

        return frame;
    }

    static bool ParseIssObservation(const char* json, IssObservation& observation)
    {
        if (json == nullptr)
        {
            return false;
        }

        const char* latitudeKey = std::strstr(json, "\"latitude\":");
        const char* longitudeKey = std::strstr(json, "\"longitude\":");
        const char* altitudeKey = std::strstr(json, "\"altitude\":");
        const char* velocityKey = std::strstr(json, "\"velocity\":");
        const char* visibilityKey = std::strstr(json, "\"visibility\":\"");

        if (latitudeKey == nullptr || longitudeKey == nullptr || altitudeKey == nullptr || velocityKey == nullptr)
        {
            return false;
        }

        observation.latitude = std::strtod(latitudeKey + 11, nullptr);
        observation.longitude = std::strtod(longitudeKey + 12, nullptr);
        observation.altitudeKm = std::strtod(altitudeKey + 11, nullptr);
        observation.velocityKph = std::strtod(velocityKey + 11, nullptr);

        if (visibilityKey != nullptr)
        {
            CopyQuotedJsonValue(visibilityKey + 14, observation.visibility.data(), observation.visibility.size());
        }

        return true;
    }

    static bool ParseSatelliteObservation(const char* html, SatelliteObservation& observation)
    {
        if (html == nullptr)
        {
            return false;
        }

        const char* gifToken = std::strstr(html, "GOES19-ABI-CONUS-GEOCOLOR-");
        if (gifToken == nullptr)
        {
            return false;
        }

        const char* urlStart = gifToken;
        while (urlStart > html && *(urlStart - 1) != '"' && *(urlStart - 1) != '(')
        {
            --urlStart;
        }

        const char* urlEnd = std::strstr(gifToken, ".gif");
        if (urlEnd == nullptr)
        {
            return false;
        }

        const std::size_t urlLength = static_cast<std::size_t>((urlEnd - urlStart) + 4);
        if (urlLength + 1u >= observation.loopGifUrl.size())
        {
            return false;
        }

        std::memcpy(observation.loopGifUrl.data(), urlStart, urlLength);
        observation.loopGifUrl[urlLength] = '\0';
        std::snprintf(observation.productName.data(), observation.productName.size(), "NOAA GOES-East CONUS GeoColor");
        std::snprintf(observation.updatedText.data(), observation.updatedText.size(), "images update every 5 minutes");
        return true;
    }

    static RawFrame BuildFrameFromIssObservation(const IssObservation& observation)
    {
        const auto clampSignedField = [](double value) -> std::int16_t
        {
            const double limited = ClampValue(value, -32.0, 31.0);
            return static_cast<std::int16_t>(limited);
        };

        const auto clampUnsignedField = [](double value) -> std::uint16_t
        {
            const double limited = ClampValue(value, 1.0, 1023.0);
            return static_cast<std::uint16_t>(limited);
        };

        const std::int16_t pitchDeg = clampSignedField(observation.latitude / 2.0);
        const std::int16_t yawDeg = clampSignedField(observation.longitude / 5.625);
        const std::uint16_t rangeMeters = clampUnsignedField(observation.altitudeKm * 2.0);
        const std::uint8_t quality = static_cast<std::uint8_t>(ClampValue(observation.velocityKph / 3000.0, 0.0, 15.0));
        const std::uint8_t status = (std::strncmp(observation.visibility.data(), "visible", 7u) == 0) ? kStatusTracking : kStatusAcquiring;

        RawFrame frame{};
        frame.words[0] = kWord0ValidMask;
        frame.words[0] |= static_cast<std::uint16_t>((static_cast<std::uint16_t>(status) & 0x7u) << 12u);
        frame.words[0] |= static_cast<std::uint16_t>((1u & 0x1Fu) << 7u);
        frame.words[0] |= 1u;

        const std::uint16_t packedPitch = static_cast<std::uint16_t>(static_cast<std::uint16_t>(pitchDeg + static_cast<std::int16_t>(kPitchOffset)) & 0x3Fu);
        const std::uint16_t packedYaw = static_cast<std::uint16_t>(static_cast<std::uint16_t>(yawDeg + static_cast<std::int16_t>(kYawOffset)) & 0x3Fu);
        frame.words[1] = static_cast<std::uint16_t>((packedPitch << 10u) | (packedYaw << 4u) | (quality & 0x0Fu));

        frame.words[2] = static_cast<std::uint16_t>((rangeMeters & 0x03FFu) << 6u);
        const std::uint16_t checksum = static_cast<std::uint16_t>((frame.words[0] ^ (frame.words[1] << 1u) ^ (frame.words[2] >> 1u)) & 0x003Fu);
        frame.words[2] |= checksum;

        return frame;
    }

#ifdef _WIN32
    static bool FetchUsgsAllHourGeoJson(std::array<char, 16384>& buffer, std::size_t& bytesWritten)
    {
        bytesWritten = 0u;

        HINTERNET session = WinHttpOpen(L"TelemetrySim/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS,
                                        0);
        if (session == nullptr)
        {
            return false;
        }

        HINTERNET connection = WinHttpConnect(session, L"earthquake.usgs.gov", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connection == nullptr)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(connection,
                                               L"GET",
                                               L"/earthquakes/feed/v1.0/summary/all_hour.geojson",
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request == nullptr)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        const BOOL sendOk = WinHttpSendRequest(request,
                                               WINHTTP_NO_ADDITIONAL_HEADERS,
                                               0,
                                               WINHTTP_NO_REQUEST_DATA,
                                               0,
                                               0,
                                               0);
        if (!sendOk || !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        DWORD availableBytes = 0u;
        while (WinHttpQueryDataAvailable(request, &availableBytes) && availableBytes > 0u)
        {
            const std::size_t remaining = buffer.size() - bytesWritten - 1u;
            if (remaining == 0u)
            {
                break;
            }

            const DWORD bytesToRead = static_cast<DWORD>((availableBytes < remaining) ? availableBytes : remaining);
            DWORD bytesRead = 0u;
            if (!WinHttpReadData(request, buffer.data() + bytesWritten, bytesToRead, &bytesRead))
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }

            bytesWritten += bytesRead;
            if (bytesWritten >= buffer.size() - 1u)
            {
                break;
            }
        }

        buffer[bytesWritten] = '\0';
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return bytesWritten > 0u;
    }

    static bool FetchIssCurrentPosition(std::array<char, 4096>& buffer, std::size_t& bytesWritten)
    {
        bytesWritten = 0u;

        HINTERNET session = WinHttpOpen(L"TelemetrySim/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS,
                                        0);
        if (session == nullptr)
        {
            return false;
        }

        HINTERNET connection = WinHttpConnect(session, L"api.wheretheiss.at", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connection == nullptr)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(connection,
                                               L"GET",
                                               L"/v1/satellites/25544",
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request == nullptr)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        const BOOL sendOk = WinHttpSendRequest(request,
                                               WINHTTP_NO_ADDITIONAL_HEADERS,
                                               0,
                                               WINHTTP_NO_REQUEST_DATA,
                                               0,
                                               0,
                                               0);
        if (!sendOk || !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            return false;
        }

        DWORD availableBytes = 0u;
        while (WinHttpQueryDataAvailable(request, &availableBytes) && availableBytes > 0u)
        {
            const std::size_t remaining = buffer.size() - bytesWritten - 1u;
            if (remaining == 0u)
            {
                break;
            }

            const DWORD bytesToRead = static_cast<DWORD>((availableBytes < remaining) ? availableBytes : remaining);
            DWORD bytesRead = 0u;
            if (!WinHttpReadData(request, buffer.data() + bytesWritten, bytesToRead, &bytesRead))
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                return false;
            }

            bytesWritten += bytesRead;
            if (bytesWritten >= buffer.size() - 1u)
            {
                break;
            }
        }

        buffer[bytesWritten] = '\0';
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return bytesWritten > 0u;
    }
#endif

    // Fixed-size circular buffer: a deterministic queue with constant memory footprint.
    class TelemetryQueue
    {
    public:
        bool push(const RawFrame& frame)
        {
            if (count_ == kQueueDepth)
            {
                return false;
            }

            storage_[head_] = frame;
            head_ = (head_ + 1u) % kQueueDepth;
            ++count_;
            return true;
        }

        bool pop(RawFrame& frame)
        {
            if (count_ == 0u)
            {
                return false;
            }

            frame = storage_[tail_];
            tail_ = (tail_ + 1u) % kQueueDepth;
            --count_;
            return true;
        }

        bool empty() const
        {
            return count_ == 0u;
        }

    private:
        std::array<RawFrame, kQueueDepth> storage_{};
        std::size_t head_ = 0u;
        std::size_t tail_ = 0u;
        std::size_t count_ = 0u;
    };

    class SensorManager
    {
    public:
        static constexpr std::uint32_t kWatchdogTimeoutTicks = 3u;

        static RawFrame PackFrame(std::uint8_t status,
                                  std::uint8_t targetId,
                                  std::int16_t pitchDeg,
                                  std::int16_t yawDeg,
                                  std::uint16_t distanceMeters,
                                  std::uint8_t sequence,
                                  std::uint8_t quality,
                                  bool corruptChecksum = false)
        {
            RawFrame frame{};

            // Word 0 layout:
            // bit 15: valid marker
            // bits 14..12: status
            // bits 11..7: target ID
            // bits 6..0: sequence number
            frame.words[0] = kWord0ValidMask;
            frame.words[0] |= static_cast<std::uint16_t>((static_cast<std::uint16_t>(status) & 0x7u) << 12u);
            frame.words[0] |= static_cast<std::uint16_t>((static_cast<std::uint16_t>(targetId) & 0x1Fu) << 7u);
            frame.words[0] |= static_cast<std::uint16_t>(sequence & 0x7Fu);

            // Word 1 layout:
            // bits 15..10: pitch as unsigned offset binary to keep the packet format compact.
            // bits 9..4: yaw as unsigned offset binary.
            // bits 3..0: quality.
            const std::uint16_t packedPitch = static_cast<std::uint16_t>(static_cast<std::uint16_t>(pitchDeg + static_cast<std::int16_t>(kPitchOffset)) & 0x3Fu);
            const std::uint16_t packedYaw   = static_cast<std::uint16_t>(static_cast<std::uint16_t>(yawDeg + static_cast<std::int16_t>(kYawOffset)) & 0x3Fu);
            frame.words[1] = static_cast<std::uint16_t>((packedPitch << 10u) | (packedYaw << 4u) | (quality & 0x0Fu));

            // Word 2 layout:
            // bits 15..6: distance in meters, scaled directly for simplicity.
            // bits 5..0: simple checksum over the previous words.
            frame.words[2] = static_cast<std::uint16_t>((distanceMeters & 0x03FFu) << 6u);
            std::uint16_t checksum = ComputeChecksum(frame.words[0], frame.words[1], frame.words[2]);
            if (corruptChecksum)
            {
                checksum ^= 0x003Fu;
            }

            frame.words[2] |= static_cast<std::uint16_t>(checksum & 0x003Fu);
            return frame;
        }

        void EnqueueFrame(const RawFrame& frame)
        {
            if (!queue_.push(frame))
            {
                // Deterministic overflow handling: increment a counter rather than reallocating.
                ++overflowCount_;
            }
        }

        void ProcessOnce()
        {
            RawFrame frame{};
            if (!queue_.pop(frame))
            {
                // No frame available this cycle; a watchdog-style timeout models a missed real-time update.
                ++ticksSinceLastFrame_;
                if (ticksSinceLastFrame_ >= kWatchdogTimeoutTicks)
                {
                    state_ = TrackState::FAULT;
                }
                return;
            }

            ticksSinceLastFrame_ = 0u;
            const DecodedFrame decoded = DecodeFrame(frame);
            UpdateState(decoded.checksumValid, decoded.target.valid);

            if (decoded.checksumValid && decoded.target.valid)
            {
                currentTarget_ = decoded.target;
                lastSequence_ = decoded.sequence;
            }
            else if (!decoded.checksumValid)
            {
                ++checksumFaultCount_;
            }

            ++processedFrames_;
        }

        TrackState state() const
        {
            return state_;
        }

        const Target& target() const
        {
            return currentTarget_;
        }

        std::uint32_t processedFrames() const
        {
            return processedFrames_;
        }

        std::uint32_t ticksSinceLastFrame() const
        {
            return ticksSinceLastFrame_;
        }

        std::uint32_t checksumFaults() const
        {
            return checksumFaultCount_;
        }

        std::uint32_t queueOverflows() const
        {
            return overflowCount_;
        }

    private:
        static std::uint16_t ComputeChecksum(std::uint16_t word0,
                                             std::uint16_t word1,
                                             std::uint16_t word2WithoutChecksum)
        {
            // Small checksum to model a lightweight embedded integrity check.
            // The reduction keeps the implementation deterministic and inexpensive.
            const std::uint16_t mixed = static_cast<std::uint16_t>((word0 ^ (word1 << 1u) ^ (word2WithoutChecksum >> 1u)) & 0x003Fu);
            return mixed;
        }

        static std::uint16_t ExtractBits(std::uint16_t value, std::uint16_t mask, std::uint8_t shift)
        {
            return static_cast<std::uint16_t>((value & mask) >> shift);
        }

        static std::int16_t DecodeSigned6(std::uint16_t packed)
        {
            // Convert offset-binary back to signed degrees.
            return static_cast<std::int16_t>(static_cast<std::int16_t>(packed) - static_cast<std::int16_t>(kPitchOffset));
        }

        static DecodedFrame DecodeFrame(const RawFrame& frame)
        {
            DecodedFrame decoded{};

            const std::uint16_t word0 = frame.words[0];
            const std::uint16_t word1 = frame.words[1];
            const std::uint16_t word2 = frame.words[2];

            const std::uint16_t checksumField = static_cast<std::uint16_t>(word2 & kWord2ChecksumMask);
            const std::uint16_t checksumExpected = ComputeChecksum(word0, word1, static_cast<std::uint16_t>(word2 & static_cast<std::uint16_t>(~kWord2ChecksumMask)));
            decoded.checksumValid = (checksumField == checksumExpected);

            decoded.status = static_cast<std::uint8_t>(ExtractBits(word0, kWord0StatusMask, 12u));
            decoded.sequence = static_cast<std::uint8_t>(ExtractBits(word0, kWord0SequenceMask, 0u));

            decoded.target.id = static_cast<std::uint8_t>(ExtractBits(word0, kWord0TargetMask, 7u));
            const std::uint16_t packedPitch = ExtractBits(word1, kWord1PitchMask, 10u);
            const std::uint16_t packedYaw = ExtractBits(word1, kWord1YawMask, 4u);

            decoded.target.pitchDeg = DecodeSigned6(packedPitch);
            decoded.target.yawDeg = DecodeSigned6(packedYaw);
            decoded.target.distanceMeters = static_cast<std::uint16_t>(ExtractBits(word2, kWord2DistanceMask, 6u));
            decoded.target.quality = static_cast<std::uint8_t>(ExtractBits(word1, kWord1QualityMask, 0u));
            decoded.target.valid = (decoded.target.quality >= 2u) && (decoded.target.distanceMeters > 0u);

            return decoded;
        }

        void UpdateState(bool checksumValid, bool frameHasTrackableTarget)
        {
            switch (state_)
            {
                case TrackState::IDLE:
                    if (checksumValid && frameHasTrackableTarget)
                    {
                        state_ = TrackState::ACQUIRING;
                    }
                    break;

                case TrackState::ACQUIRING:
                    if (!checksumValid)
                    {
                        state_ = TrackState::FAULT;
                    }
                    else if (frameHasTrackableTarget)
                    {
                        state_ = TrackState::TRACKING;
                    }
                    break;

                case TrackState::TRACKING:
                    if (!checksumValid)
                    {
                        state_ = TrackState::FAULT;
                    }
                    else if (!frameHasTrackableTarget)
                    {
                        state_ = TrackState::ACQUIRING;
                    }
                    break;

                case TrackState::FAULT:
                    // In a real embedded controller this would typically require a supervisory reset.
                    // We model a deterministic auto-recovery after a valid frame arrives.
                    if (checksumValid)
                    {
                        state_ = frameHasTrackableTarget ? TrackState::TRACKING : TrackState::ACQUIRING;
                    }
                    break;
            }
        }

        TelemetryQueue queue_{};
        TrackState state_ = TrackState::IDLE;
        Target currentTarget_{};
        std::uint32_t processedFrames_ = 0u;
        std::uint32_t checksumFaultCount_ = 0u;
        std::uint32_t overflowCount_ = 0u;
        std::uint32_t ticksSinceLastFrame_ = 0u;
        std::uint8_t lastSequence_ = 0u;
    };

    static const char* ToString(TrackState state)
    {
        switch (state)
        {
            case TrackState::IDLE:      return "IDLE";
            case TrackState::ACQUIRING: return "ACQUIRING";
            case TrackState::TRACKING:  return "TRACKING";
            case TrackState::FAULT:     return "FAULT";
        }

        return "UNKNOWN";
    }

    enum class LiveSourceMode : std::uint8_t
    {
        Synthetic = 0u,
        Usgs,
        Iss,
        Satellite
    };

    struct GuiAppState
    {
        SensorManager manager{};
        LiveSourceMode source = LiveSourceMode::Synthetic;
        std::uint32_t tick = 0u;
        SourceStats currentStats{};
        static constexpr std::size_t kGraphSamples = 240u;
        std::array<std::array<std::uint32_t, kGraphSamples>, 4u> metricHistory{};
        std::array<std::uint32_t, kGraphSamples> framesHistory{};
        std::array<std::uint32_t, kGraphSamples> faultsHistory{};
        std::array<std::uint32_t, kGraphSamples> missedHistory{};
        std::array<std::uint32_t, kGraphSamples> rangeHistory{};
        std::size_t historyCount = 0u;
        std::size_t historyHead = 0u;
        std::array<char, 16384> logBuffer{};
        std::size_t logLength = 0u;
        HWND hwndState = nullptr;
        HWND hwndFrames = nullptr;
        HWND hwndMissed = nullptr;
        HWND hwndFaults = nullptr;
        HWND hwndOverflows = nullptr;
        HWND hwndTarget = nullptr;
        HWND hwndLog = nullptr;
        HWND hwndSourceCombo = nullptr;
    };

    static const char* ToString(LiveSourceMode source)
    {
        switch (source)
        {
            case LiveSourceMode::Synthetic: return "Synthetic";
            case LiveSourceMode::Usgs:      return "USGS";
            case LiveSourceMode::Iss:       return "ISS";
            case LiveSourceMode::Satellite: return "Satellite Loop";
        }

        return "Unknown";
    }

    static void ClearLog(GuiAppState& app)
    {
        app.logBuffer.fill('\0');
        app.logLength = 0u;
    }

    static void AppendLogLine(GuiAppState& app, const char* text)
    {
        if (text == nullptr)
        {
            return;
        }

        const std::size_t textLength = std::strlen(text);
        const std::size_t required = textLength + 2u;
        if (required >= app.logBuffer.size())
        {
            return;
        }

        if (app.logLength + required >= app.logBuffer.size())
        {
            ClearLog(app);
        }

        std::memcpy(app.logBuffer.data() + app.logLength, text, textLength);
        app.logLength += textLength;
        app.logBuffer[app.logLength++] = '\r';
        app.logBuffer[app.logLength++] = '\n';
        app.logBuffer[app.logLength] = '\0';
    }

    static void ResetGraphHistory(GuiAppState& app)
    {
        app.historyCount = 0u;
        app.historyHead = 0u;
        for (auto& series : app.metricHistory)
        {
            series.fill(0u);
        }
    }

    static void PushHistorySample(GuiAppState& app)
    {
        for (std::size_t seriesIndex = 0u; seriesIndex < app.currentStats.metricValues.size(); ++seriesIndex)
        {
            app.metricHistory[seriesIndex][app.historyHead] = app.currentStats.metricValues[seriesIndex];
        }

        app.historyHead = (app.historyHead + 1u) % GuiAppState::kGraphSamples;
        if (app.historyCount < GuiAppState::kGraphSamples)
        {
            ++app.historyCount;
        }
    }

    static std::uint32_t MaxHistoryValue(const GuiAppState& app, const std::array<std::uint32_t, GuiAppState::kGraphSamples>& values)
    {
        std::uint32_t maximum = 1u;
        for (std::size_t index = 0u; index < app.historyCount; ++index)
        {
            const std::size_t sampleIndex = (app.historyHead + GuiAppState::kGraphSamples - app.historyCount + index) % GuiAppState::kGraphSamples;
            if (values[sampleIndex] > maximum)
            {
                maximum = values[sampleIndex];
            }
        }

        return maximum;
    }

    static void DrawGraphSeries(HDC deviceContext,
                                const RECT& bounds,
                                const GuiAppState& app,
                                const std::array<std::uint32_t, GuiAppState::kGraphSamples>& values,
                                COLORREF lineColor,
                                COLORREF fillColor,
                                const char* title)
    {
        const HBRUSH background = CreateSolidBrush(RGB(250, 250, 250));
        FillRect(deviceContext, &bounds, background);
        DeleteObject(background);

        const HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(180, 180, 180));
        const HPEN linePen = CreatePen(PS_SOLID, 2, lineColor);
        const HPEN fillPen = CreatePen(PS_SOLID, 1, fillColor);
        const HBRUSH noBrush = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        const HGDIOBJ oldBrush = SelectObject(deviceContext, noBrush);
        const HGDIOBJ oldBorder = SelectObject(deviceContext, borderPen);

        Rectangle(deviceContext, bounds.left, bounds.top, bounds.right, bounds.bottom);

        SetBkMode(deviceContext, TRANSPARENT);
        SetTextColor(deviceContext, RGB(30, 30, 30));
        TextOutA(deviceContext, bounds.left + 6, bounds.top + 4, title, static_cast<int>(std::strlen(title)));

        const int plotLeft = bounds.left + 8;
        const int plotTop = bounds.top + 24;
        const int plotRight = bounds.right - 8;
        const int plotBottom = bounds.bottom - 10;
        const int plotWidth = plotRight - plotLeft;
        const int plotHeight = plotBottom - plotTop;

        MoveToEx(deviceContext, plotLeft, plotBottom, nullptr);
        LineTo(deviceContext, plotRight, plotBottom);
        MoveToEx(deviceContext, plotLeft, plotTop, nullptr);
        LineTo(deviceContext, plotLeft, plotBottom);

        const std::uint32_t maximum = MaxHistoryValue(app, values);
        if (app.historyCount >= 2u)
        {
            SelectObject(deviceContext, linePen);
            const std::size_t startIndex = (app.historyHead + GuiAppState::kGraphSamples - app.historyCount) % GuiAppState::kGraphSamples;
            for (std::size_t index = 0u; index < app.historyCount; ++index)
            {
                const std::size_t sampleIndex = (startIndex + index) % GuiAppState::kGraphSamples;
                const int x = plotLeft + static_cast<int>((index * plotWidth) / (app.historyCount - 1u));
                const std::uint32_t sample = values[sampleIndex];
                const int y = plotBottom - static_cast<int>((sample * plotHeight) / maximum);
                if (index == 0u)
                {
                    MoveToEx(deviceContext, x, y, nullptr);
                }
                else
                {
                    LineTo(deviceContext, x, y);
                }
            }
        }

        SelectObject(deviceContext, fillPen);
        MoveToEx(deviceContext, plotLeft, plotBottom, nullptr);

        SelectObject(deviceContext, oldBorder);
        SelectObject(deviceContext, oldBrush);
        DeleteObject(borderPen);
        DeleteObject(linePen);
        DeleteObject(fillPen);
    }

    static void PaintGraphs(HWND hwnd, HDC deviceContext, const GuiAppState& app)
    {
        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);

        const int graphTop = 140;
        const int graphMargin = 12;
        const int graphHeight = 160;
        const int graphWidth = (clientRect.right - (graphMargin * 3)) / 2;

        RECT framesRect{graphMargin, graphTop, graphMargin + graphWidth, graphTop + graphHeight};
        RECT faultsRect{graphMargin * 2 + graphWidth, graphTop, graphMargin * 2 + graphWidth * 2, graphTop + graphHeight};
        RECT missedRect{graphMargin, graphTop + graphHeight + graphMargin, graphMargin + graphWidth, graphTop + graphHeight * 2 + graphMargin};
        RECT rangeRect{graphMargin * 2 + graphWidth, graphTop + graphHeight + graphMargin, graphMargin * 2 + graphWidth * 2, graphTop + graphHeight * 2 + graphMargin};

        DrawGraphSeries(deviceContext, framesRect, app, app.metricHistory[0], RGB(0, 102, 204), RGB(220, 235, 255), app.currentStats.metricLabels[0].data());
        DrawGraphSeries(deviceContext, faultsRect, app, app.metricHistory[1], RGB(204, 0, 0), RGB(255, 230, 230), app.currentStats.metricLabels[1].data());
        DrawGraphSeries(deviceContext, missedRect, app, app.metricHistory[2], RGB(240, 140, 0), RGB(255, 242, 224), app.currentStats.metricLabels[2].data());
        DrawGraphSeries(deviceContext, rangeRect, app, app.metricHistory[3], RGB(0, 150, 90), RGB(225, 250, 236), app.currentStats.metricLabels[3].data());
    }

    static void FormatStateLine(const GuiAppState& app, char* output, std::size_t outputSize)
    {
        if (output == nullptr || outputSize == 0u)
        {
            return;
        }

        std::snprintf(output,
                      outputSize,
                      "State=%s | Source=%s | Tick=%u | %s",
                      ToString(app.manager.state()),
                      ToString(app.source),
                      static_cast<unsigned>(app.tick),
                      app.currentStats.summary.data());
    }

    static bool BuildSyntheticFrame(std::uint32_t tick, RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
        const bool corruptChecksum = (tick % 11u) == 7u;
        const std::uint8_t status = (tick == 0u) ? kStatusIdle : ((tick < 3u) ? kStatusAcquiring : kStatusTracking);
        const std::uint16_t distance = static_cast<std::uint16_t>(100u + (tick % 20u));
        const std::int16_t pitch = static_cast<std::int16_t>((tick % 13u) - 6);
        const std::int16_t yaw = static_cast<std::int16_t>(((tick * 2u) % 13u) - 6);

        frame = SensorManager::PackFrame(status,
                                         1u,
                                         pitch,
                                         yaw,
                                         distance,
                                         static_cast<std::uint8_t>(tick & 0x7Fu),
                                         4u,
                                         corruptChecksum);

        std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Status");
        std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Pitch+32");
        std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Yaw+32");
        std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "Range m");
        stats.metricValues[0] = status;
        stats.metricValues[1] = static_cast<std::uint32_t>(pitch + 32);
        stats.metricValues[2] = static_cast<std::uint32_t>(yaw + 32);
        stats.metricValues[3] = distance;
        std::snprintf(stats.summary.data(), stats.summary.size(),
                  "Synthetic packet | status=%u pitch=%d yaw=%d range=%u checksum=%s",
                  static_cast<unsigned>(status),
                  static_cast<int>(pitch),
                  static_cast<int>(yaw),
                  static_cast<unsigned>(distance),
                  corruptChecksum ? "bad" : "ok");

        std::snprintf(info,
                      infoSize,
                      "Synthetic tick %u | status=%u | pitch=%d | yaw=%d | range=%u | checksum=%s",
                      static_cast<unsigned>(tick),
                      static_cast<unsigned>(status),
                      static_cast<int>(pitch),
                      static_cast<int>(yaw),
                      static_cast<unsigned>(distance),
                      corruptChecksum ? "bad" : "ok");
        return true;
    }

    static bool BuildUsgsFrame(RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
#ifdef _WIN32
        std::array<char, 16384> jsonBuffer{};
        std::size_t jsonBytes = 0u;
        UsgsObservation observation{};

        if (!FetchUsgsAllHourGeoJson(jsonBuffer, jsonBytes) || !ParseUsgsObservation(jsonBuffer.data(), observation))
        {
            std::snprintf(info, infoSize, "USGS fetch failed");
            return false;
        }

        frame = BuildFrameFromUsgsObservation(observation);
    std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Mag x100");
    std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Lat+90 x100");
    std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Lon+180 x100");
    std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "Depth x10");
    stats.metricValues[0] = static_cast<std::uint32_t>(observation.magnitude * 100.0);
    stats.metricValues[1] = static_cast<std::uint32_t>((observation.latitude + 90.0) * 100.0);
    stats.metricValues[2] = static_cast<std::uint32_t>((observation.longitude + 180.0) * 100.0);
    stats.metricValues[3] = static_cast<std::uint32_t>(observation.depthKm * 10.0);
    std::snprintf(stats.summary.data(), stats.summary.size(),
              "USGS quake | M%.1f | %s | lat=%.3f lon=%.3f depth=%.1f km",
              observation.magnitude,
              observation.place.data(),
              observation.latitude,
              observation.longitude,
              observation.depthKm);
        std::snprintf(info,
                      infoSize,
                      "USGS M%.1f | %s | lat=%.3f lon=%.3f depth=%.1f km",
                      observation.magnitude,
                      observation.place.data(),
                      observation.latitude,
                      observation.longitude,
                      observation.depthKm);
        return true;
#else
        (void)frame;
        std::snprintf(info, infoSize, "USGS live fetch is only available on Windows");
        return false;
#endif
    }

    static bool BuildIssFrame(RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
#ifdef _WIN32
        std::array<char, 4096> jsonBuffer{};
        std::size_t jsonBytes = 0u;
        IssObservation observation{};

        if (!FetchIssCurrentPosition(jsonBuffer, jsonBytes) || !ParseIssObservation(jsonBuffer.data(), observation))
        {
            std::snprintf(info, infoSize, "ISS fetch failed");
            return false;
        }

        frame = BuildFrameFromIssObservation(observation);
    std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Lat+90 x100");
    std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Lon+180 x100");
    std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Alt x100");
    std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "Vel kph");
    stats.metricValues[0] = static_cast<std::uint32_t>((observation.latitude + 90.0) * 100.0);
    stats.metricValues[1] = static_cast<std::uint32_t>((observation.longitude + 180.0) * 100.0);
    stats.metricValues[2] = static_cast<std::uint32_t>(observation.altitudeKm * 100.0);
    stats.metricValues[3] = static_cast<std::uint32_t>(observation.velocityKph);
    std::snprintf(stats.summary.data(), stats.summary.size(),
              "ISS | lat=%.3f lon=%.3f alt=%.1f km vel=%.1f kph vis=%s",
              observation.latitude,
              observation.longitude,
              observation.altitudeKm,
              observation.velocityKph,
              observation.visibility.data());
        std::snprintf(info,
                      infoSize,
                      "ISS lat=%.3f lon=%.3f alt=%.1f km vel=%.1f kph vis=%s",
                      observation.latitude,
                      observation.longitude,
                      observation.altitudeKm,
                      observation.velocityKph,
                      observation.visibility.data());
        return true;
#else
        (void)frame;
        std::snprintf(info, infoSize, "ISS live fetch is only available on Windows");
        return false;
#endif
    }

    static bool BuildSatelliteFrame(RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
#ifdef _WIN32
        std::array<char, 32768> htmlBuffer{};
        std::size_t bytesWritten = 0u;
        SatelliteObservation observation{};

        HINTERNET session = WinHttpOpen(L"TelemetrySim/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS,
                                        0);
        if (session == nullptr)
        {
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        HINTERNET connection = WinHttpConnect(session, L"www.star.nesdis.noaa.gov", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (connection == nullptr)
        {
            WinHttpCloseHandle(session);
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        HINTERNET request = WinHttpOpenRequest(connection,
                                               L"GET",
                                               L"/GOES/conus.php?sat=G19",
                                               nullptr,
                                               WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (request == nullptr)
        {
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            std::snprintf(info, infoSize, "Satellite page fetch failed");
            return false;
        }

        DWORD availableBytes = 0u;
        while (WinHttpQueryDataAvailable(request, &availableBytes) && availableBytes > 0u)
        {
            const std::size_t remaining = htmlBuffer.size() - bytesWritten - 1u;
            if (remaining == 0u)
            {
                break;
            }

            const DWORD bytesToRead = static_cast<DWORD>((availableBytes < remaining) ? availableBytes : remaining);
            DWORD bytesRead = 0u;
            if (!WinHttpReadData(request, htmlBuffer.data() + bytesWritten, bytesToRead, &bytesRead))
            {
                WinHttpCloseHandle(request);
                WinHttpCloseHandle(connection);
                WinHttpCloseHandle(session);
                std::snprintf(info, infoSize, "Satellite page fetch failed");
                return false;
            }

            bytesWritten += bytesRead;
            if (bytesWritten >= htmlBuffer.size() - 1u)
            {
                break;
            }
        }

        htmlBuffer[bytesWritten] = '\0';
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);

        if (!ParseSatelliteObservation(htmlBuffer.data(), observation))
        {
            std::snprintf(info, infoSize, "Satellite loop parse failed");
            return false;
        }

        frame = SensorManager::PackFrame(kStatusTracking,
                                         1u,
                                         static_cast<std::int16_t>((bytesWritten % 13u) - 6u),
                                         static_cast<std::int16_t>(((bytesWritten * 3u) % 13u) - 6u),
                                         150u,
                                         static_cast<std::uint8_t>(bytesWritten & 0x7Fu),
                                         5u,
                                         false);

        const char* filename = std::strrchr(observation.loopGifUrl.data(), '/');
        filename = (filename == nullptr) ? observation.loopGifUrl.data() : (filename + 1);
        const std::uint32_t loopStart = static_cast<std::uint32_t>(std::strtoul(filename, nullptr, 10));
        const char* secondDash = std::strchr(filename, '-');
        const std::uint32_t loopEnd = (secondDash == nullptr) ? 0u : static_cast<std::uint32_t>(std::strtoul(secondDash + 1, nullptr, 10));

        std::snprintf(stats.metricLabels[0].data(), stats.metricLabels[0].size(), "Loop start");
        std::snprintf(stats.metricLabels[1].data(), stats.metricLabels[1].size(), "Loop end");
        std::snprintf(stats.metricLabels[2].data(), stats.metricLabels[2].size(), "Fetch bytes");
        std::snprintf(stats.metricLabels[3].data(), stats.metricLabels[3].size(), "URL len");
        stats.metricValues[0] = loopStart;
        stats.metricValues[1] = loopEnd;
        stats.metricValues[2] = static_cast<std::uint32_t>(bytesWritten);
        stats.metricValues[3] = static_cast<std::uint32_t>(observation.loopGifUrl.size());
        std::snprintf(stats.summary.data(), stats.summary.size(),
                  "%s | %s | %s",
                  observation.productName.data(),
                  observation.updatedText.data(),
                  kSatelliteLoopPageUrl);

        std::snprintf(info,
                      infoSize,
                      "Satellite loop | %s | %s | %s",
                      observation.productName.data(),
                      observation.updatedText.data(),
                      kSatelliteLoopPageUrl);
        return true;
#else
        (void)frame;
        std::snprintf(info, infoSize, "Satellite loop is only available on Windows in this build");
        return false;
#endif
    }

    static bool BuildFrameForSource(LiveSourceMode source, std::uint32_t tick, RawFrame& frame, SourceStats& stats, char* info, std::size_t infoSize)
    {
        switch (source)
        {
            case LiveSourceMode::Synthetic:
                return BuildSyntheticFrame(tick, frame, stats, info, infoSize);
            case LiveSourceMode::Usgs:
                return BuildUsgsFrame(frame, stats, info, infoSize);
            case LiveSourceMode::Iss:
                return BuildIssFrame(frame, stats, info, infoSize);
            case LiveSourceMode::Satellite:
                return BuildSatelliteFrame(frame, stats, info, infoSize);
        }

        std::snprintf(info, infoSize, "Unknown source");
        return false;
    }
} // namespace telemetry

namespace
{
#ifdef _WIN32
    constexpr UINT_PTR kGuiTimerId = 1u;
    constexpr int kSourceComboId = 1001;

    static void UpdateGuiLayout(HWND hwnd, telemetry::GuiAppState* app)
    {
        if (app == nullptr)
        {
            return;
        }

        RECT clientRect{};
        GetClientRect(hwnd, &clientRect);

        const int margin = 12;
        const int rowHeight = 24;
        const int labelWidth = 160;
        const int valueWidth = 240;
        const int controlWidth = 220;
        const int top = 12;
        const int left = 12;
        const int valueX = left + labelWidth;
        const int rightColX = valueX + valueWidth + 20;

        MoveWindow(app->hwndSourceCombo, valueX, top - 2, controlWidth, 200, TRUE);
        MoveWindow(app->hwndState, valueX, top + rowHeight, valueWidth, rowHeight, TRUE);
        MoveWindow(app->hwndFrames, valueX, top + rowHeight * 2, valueWidth, rowHeight, TRUE);
        MoveWindow(app->hwndMissed, valueX, top + rowHeight * 3, valueWidth, rowHeight, TRUE);
        MoveWindow(app->hwndFaults, rightColX, top + rowHeight, valueWidth, rowHeight, TRUE);
        MoveWindow(app->hwndOverflows, rightColX, top + rowHeight * 2, valueWidth, rowHeight, TRUE);
        MoveWindow(app->hwndTarget, rightColX, top + rowHeight * 3, valueWidth, rowHeight, TRUE);

        const int logTop = 480;
        const int logHeight = clientRect.bottom - logTop - margin;
        const int logWidth = clientRect.right - (left * 2);
        MoveWindow(app->hwndLog, left, logTop, logWidth, logHeight, TRUE);
    }

    static void UpdateGuiValues(HWND hwnd, telemetry::GuiAppState* app, const char* infoLine)
    {
        if (app == nullptr)
        {
            return;
        }

        char statusLine[256]{};
        telemetry::FormatStateLine(*app, statusLine, sizeof(statusLine));

        char stateText[256]{};
        std::snprintf(stateText, sizeof(stateText), "State: %s", telemetry::ToString(app->manager.state()));
        SetWindowTextA(app->hwndState, stateText);

        char framesText[256]{};
        std::snprintf(framesText,
                      sizeof(framesText),
                      "Frames processed: %u",
                      static_cast<unsigned>(app->manager.processedFrames()));
        SetWindowTextA(app->hwndFrames, framesText);

        char missedText[256]{};
        std::snprintf(missedText,
                      sizeof(missedText),
                      "Missed ticks: %u",
                      static_cast<unsigned>(app->manager.ticksSinceLastFrame()));
        SetWindowTextA(app->hwndMissed, missedText);

        char faultsText[256]{};
        std::snprintf(faultsText,
                      sizeof(faultsText),
                      "Checksum faults: %u",
                      static_cast<unsigned>(app->manager.checksumFaults()));
        SetWindowTextA(app->hwndFaults, faultsText);

        char overflowsText[256]{};
        std::snprintf(overflowsText,
                      sizeof(overflowsText),
                      "Queue overflows: %u",
                      static_cast<unsigned>(app->manager.queueOverflows()));
        SetWindowTextA(app->hwndOverflows, overflowsText);

        SetWindowTextA(app->hwndTarget, app->currentStats.summary.data());

        if (infoLine != nullptr && *infoLine != '\0')
        {
            telemetry::AppendLogLine(*app, infoLine);
        }

        telemetry::AppendLogLine(*app, statusLine);
        SetWindowTextA(app->hwndLog, app->logBuffer.data());
        const LRESULT logLength = SendMessageA(app->hwndLog, WM_GETTEXTLENGTH, 0, 0);
        SendMessageA(app->hwndLog, EM_SETSEL, static_cast<WPARAM>(logLength), static_cast<LPARAM>(logLength));
        SendMessageA(app->hwndLog, EM_SCROLLCARET, 0, 0);
    }

    static void RunGuiTick(HWND hwnd, telemetry::GuiAppState* app)
    {
        if (app == nullptr)
        {
            return;
        }

        telemetry::RawFrame frame{};
        char infoLine[256]{};
        const bool haveFrame = telemetry::BuildFrameForSource(app->source, app->tick, frame, app->currentStats, infoLine, sizeof(infoLine));
        if (haveFrame)
        {
            app->manager.EnqueueFrame(frame);
        }

        app->manager.ProcessOnce();
        telemetry::PushHistorySample(*app);
        UpdateGuiValues(hwnd, app, infoLine);
        InvalidateRect(hwnd, nullptr, FALSE);
        ++app->tick;
    }

    static LRESULT CALLBACK GuiWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        telemetry::GuiAppState* app = reinterpret_cast<telemetry::GuiAppState*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));

        switch (message)
        {
            case WM_CREATE:
            {
                auto* createStruct = reinterpret_cast<CREATESTRUCTA*>(lParam);
                app = reinterpret_cast<telemetry::GuiAppState*>(createStruct->lpCreateParams);
                SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));

                CreateWindowExA(0, "STATIC", "Live source:", WS_CHILD | WS_VISIBLE, 12, 12, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndSourceCombo = CreateWindowExA(0,
                                                       "COMBOBOX",
                                                       nullptr,
                                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                                       132,
                                                       10,
                                                       220,
                                                       400,
                                                       hwnd,
                                                       reinterpret_cast<HMENU>(kSourceComboId),
                                                       createStruct->hInstance,
                                                       nullptr);

                CreateWindowExA(0, "STATIC", "State:", WS_CHILD | WS_VISIBLE, 12, 36, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndState = CreateWindowExA(0, "STATIC", "State: IDLE", WS_CHILD | WS_VISIBLE, 132, 36, 360, 20, hwnd, nullptr, createStruct->hInstance, nullptr);

                CreateWindowExA(0, "STATIC", "Frames processed:", WS_CHILD | WS_VISIBLE, 12, 60, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndFrames = CreateWindowExA(0, "STATIC", "Frames processed: 0", WS_CHILD | WS_VISIBLE, 132, 60, 360, 20, hwnd, nullptr, createStruct->hInstance, nullptr);

                CreateWindowExA(0, "STATIC", "Missed ticks:", WS_CHILD | WS_VISIBLE, 12, 84, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndMissed = CreateWindowExA(0, "STATIC", "Missed ticks: 0", WS_CHILD | WS_VISIBLE, 132, 84, 360, 20, hwnd, nullptr, createStruct->hInstance, nullptr);

                CreateWindowExA(0, "STATIC", "Checksum faults:", WS_CHILD | WS_VISIBLE, 520, 36, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndFaults = CreateWindowExA(0, "STATIC", "Checksum faults: 0", WS_CHILD | WS_VISIBLE, 640, 36, 360, 20, hwnd, nullptr, createStruct->hInstance, nullptr);

                CreateWindowExA(0, "STATIC", "Queue overflows:", WS_CHILD | WS_VISIBLE, 520, 60, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndOverflows = CreateWindowExA(0, "STATIC", "Queue overflows: 0", WS_CHILD | WS_VISIBLE, 640, 60, 360, 20, hwnd, nullptr, createStruct->hInstance, nullptr);

                CreateWindowExA(0, "STATIC", "Source summary:", WS_CHILD | WS_VISIBLE, 520, 84, 120, 20, hwnd, nullptr, createStruct->hInstance, nullptr);
                app->hwndTarget = CreateWindowExA(0, "STATIC", "Waiting for first source sample...", WS_CHILD | WS_VISIBLE, 640, 84, 360, 20, hwnd, nullptr, createStruct->hInstance, nullptr);

                app->hwndLog = CreateWindowExA(WS_EX_CLIENTEDGE,
                                               "EDIT",
                                               "",
                                               WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                                               12,
                                               128,
                                               940,
                                               520,
                                               hwnd,
                                               nullptr,
                                               createStruct->hInstance,
                                               nullptr);

                SendMessageA(app->hwndSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Synthetic"));
                SendMessageA(app->hwndSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("USGS"));
                SendMessageA(app->hwndSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("ISS"));
                SendMessageA(app->hwndSourceCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Satellite Loop"));
                SendMessageA(app->hwndSourceCombo, CB_SETCURSEL, 0, 0);
                ClearLog(*app);
                ResetGraphHistory(*app);
                AppendLogLine(*app, "GUI initialized");
                AppendLogLine(*app, "Choose a source from the drop-down and watch live values update.");
                SetTimer(hwnd,
                         kGuiTimerId,
                         (app->source == telemetry::LiveSourceMode::Satellite) ? 5000u : 1000u,
                         nullptr);
                UpdateGuiLayout(hwnd, app);
                RunGuiTick(hwnd, app);
                return 0;
            }

            case WM_COMMAND:
            {
                if (app != nullptr && LOWORD(wParam) == kSourceComboId && HIWORD(wParam) == CBN_SELCHANGE)
                {
                    const LRESULT selection = SendMessageA(app->hwndSourceCombo, CB_GETCURSEL, 0, 0);
                    app->source = static_cast<telemetry::LiveSourceMode>(selection);
                    ResetGraphHistory(*app);
                    app->currentStats = telemetry::SourceStats{};
                    SetTimer(hwnd,
                             kGuiTimerId,
                             (app->source == telemetry::LiveSourceMode::Satellite) ? 5000u : 1000u,
                             nullptr);
                    char messageLine[128]{};
                    std::snprintf(messageLine, sizeof(messageLine), "Source changed to %s", telemetry::ToString(app->source));
                    telemetry::AppendLogLine(*app, messageLine);
                    SetWindowTextA(app->hwndLog, app->logBuffer.data());
                    const LRESULT logLength = SendMessageA(app->hwndLog, WM_GETTEXTLENGTH, 0, 0);
                    SendMessageA(app->hwndLog, EM_SETSEL, static_cast<WPARAM>(logLength), static_cast<LPARAM>(logLength));
                    SendMessageA(app->hwndLog, EM_SCROLLCARET, 0, 0);
                }
                return 0;
            }

            case WM_PAINT:
            {
                PAINTSTRUCT paintStruct{};
                HDC deviceContext = BeginPaint(hwnd, &paintStruct);
                if (app != nullptr)
                {
                    telemetry::PaintGraphs(hwnd, deviceContext, *app);
                }
                EndPaint(hwnd, &paintStruct);
                return 0;
            }

            case WM_TIMER:
                if (wParam == kGuiTimerId)
                {
                    RunGuiTick(hwnd, app);
                }
                return 0;

            case WM_SIZE:
                UpdateGuiLayout(hwnd, app);
                return 0;

            case WM_DESTROY:
                KillTimer(hwnd, kGuiTimerId);
                PostQuitMessage(0);
                return 0;
        }

        return DefWindowProcA(hwnd, message, wParam, lParam);
    }

    static int RunGuiApp(HINSTANCE instance)
    {
        telemetry::GuiAppState app{};
        ClearLog(app);

        WNDCLASSA windowClass{};
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = GuiWndProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClass.lpszClassName = "SensorTrackingGuiWindow";

        if (RegisterClassA(&windowClass) == 0)
        {
            return 1;
        }

        HWND hwnd = CreateWindowExA(0,
                                    windowClass.lpszClassName,
                                    "Sensor & Target Tracking System",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    CW_USEDEFAULT,
                                    CW_USEDEFAULT,
                                    1000,
                                    720,
                                    nullptr,
                                    nullptr,
                                    instance,
                                    &app);
        if (hwnd == nullptr)
        {
            return 1;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        MSG message{};
        while (GetMessageA(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageA(&message);
        }

        return static_cast<int>(message.wParam);
    }
#endif
} // namespace

static const char* TrackStateToString(telemetry::TrackState state)
{
    switch (state)
    {
        case telemetry::TrackState::IDLE:      return "IDLE";
        case telemetry::TrackState::ACQUIRING: return "ACQUIRING";
        case telemetry::TrackState::TRACKING:  return "TRACKING";
        case telemetry::TrackState::FAULT:     return "FAULT";
    }

    return "UNKNOWN";
}

int main(int argc, char** argv)
{
    using namespace telemetry;

    SensorManager manager;
    bool useUsgsLiveFrame = false;
    bool useIssLiveFrame = false;
    bool useGui = false;
    std::uint32_t maxTicks = 0u;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--gui") == 0)
        {
            useGui = true;
        }
        if (std::strcmp(argv[index], "--usgs") == 0 || std::strcmp(argv[index], "--live-usgs") == 0)
        {
            useUsgsLiveFrame = true;
        }
        else if (std::strcmp(argv[index], "--iss") == 0 || std::strcmp(argv[index], "--live-iss") == 0)
        {
            useIssLiveFrame = true;
        }
        else if (std::strcmp(argv[index], "--ticks") == 0 && (index + 1) < argc)
        {
            maxTicks = static_cast<std::uint32_t>(std::strtoul(argv[index + 1], nullptr, 10));
            ++index;
        }
    }

#ifdef _WIN32
    if (useGui)
    {
        return RunGuiApp(GetModuleHandleA(nullptr));
    }
#else
    if (useGui)
    {
        std::cout << "GUI mode is only available on Windows in this build.\n";
        return 1;
    }
#endif

    if (useUsgsLiveFrame)
    {
        std::cout << "USGS live feed enabled: " << kUsgsFeedUrl << '\n';
    }
    if (useIssLiveFrame)
    {
        std::cout << "ISS live feed enabled: " << kIssFeedUrl << '\n';
    }
    if (useGui)
    {
        std::cout << "GUI mode enabled\n";
    }

    // Continuous real-time loop:
    // - each iteration represents one scheduler tick
    // - live USGS mode polls the network each tick
    // - non-USGS mode synthesizes deterministic telemetry so the state machine still moves
    std::uint32_t tick = 0u;
    while (maxTicks == 0u || tick < maxTicks)
    {
        const bool corruptChecksum = (tick % 11u) == 7u;

        if (useUsgsLiveFrame)
        {
#ifdef _WIN32
            std::array<char, 16384> jsonBuffer{};
            std::size_t jsonBytes = 0u;
            UsgsObservation observation{};

            if (FetchUsgsAllHourGeoJson(jsonBuffer, jsonBytes) && ParseUsgsObservation(jsonBuffer.data(), observation))
            {
                manager.EnqueueFrame(BuildFrameFromUsgsObservation(observation));
                std::cout << "USGS tick " << std::setw(2) << tick
                          << " | M" << observation.magnitude
                          << " | Place=" << observation.place.data()
                          << " | DepthKm=" << observation.depthKm << '\n';
            }
            else
            {
                std::cout << "USGS tick " << std::setw(2) << tick << " | fetch failed\n";
            }
#else
            std::cout << "USGS live fetch is only available on Windows in this build.\n";
#endif
        }
    else if (useIssLiveFrame)
    {
#ifdef _WIN32
        std::array<char, 4096> jsonBuffer{};
        std::size_t jsonBytes = 0u;
        IssObservation observation{};

        if (FetchIssCurrentPosition(jsonBuffer, jsonBytes) && ParseIssObservation(jsonBuffer.data(), observation))
        {
        manager.EnqueueFrame(BuildFrameFromIssObservation(observation));
        std::cout << "ISS tick " << std::setw(2) << tick
              << " | Lat=" << observation.latitude
              << " | Lon=" << observation.longitude
              << " | AltKm=" << observation.altitudeKm
              << " | VelKph=" << observation.velocityKph
              << " | Vis=" << observation.visibility.data() << '\n';
        }
        else
        {
        std::cout << "ISS tick " << std::setw(2) << tick << " | fetch failed\n";
        }
#else
        std::cout << "ISS live fetch is only available on Windows in this build.\n";
#endif
    }
    else
    {
        const std::uint8_t status = (tick == 0u) ? kStatusIdle : ((tick < 3u) ? kStatusAcquiring : kStatusTracking);
        const std::uint16_t distance = static_cast<std::uint16_t>(100u + (tick % 20u));
        const std::int16_t pitch = static_cast<std::int16_t>((tick % 13u) - 6);
        const std::int16_t yaw = static_cast<std::int16_t>(((tick * 2u) % 13u) - 6);

        manager.EnqueueFrame(SensorManager::PackFrame(status,
                              1u,
                              pitch,
                              yaw,
                              distance,
                              static_cast<std::uint8_t>(tick & 0x7Fu),
                              4u,
                              corruptChecksum));
    }

        manager.ProcessOnce();

        const Target& target = manager.target();
        std::cout << "Tick " << std::setw(2) << tick
                  << " | State=" << TrackStateToString(manager.state())
                  << " | Frames=" << manager.processedFrames()
                  << " | MissedTicks=" << manager.ticksSinceLastFrame()
                  << " | Faults=" << manager.checksumFaults()
                  << " | Overflows=" << manager.queueOverflows()
                  << " | TargetID=" << static_cast<unsigned>(target.id)
                  << " | Pitch=" << target.pitchDeg
                  << " deg | Yaw=" << target.yawDeg
                  << " deg | Range=" << target.distanceMeters
                  << " m | Quality=" << static_cast<unsigned>(target.quality)
                  << '\n';

        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    return 0;
}