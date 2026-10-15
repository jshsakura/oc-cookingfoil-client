// The part of an install that is the same for an NSP and an XCI: one NCA
// streamed into its placeholder through the buffered writer, which decrypts,
// decompresses an NCZ and keeps resume points (NczPoint) as it goes, so one
// cut off goes on from its last point. Moved here from CyberFoil's
// http_nsp.cpp (GPL-3.0, its authors' code with CookingFoil's changes) so
// HTTPXCI takes the same path instead of a copy of its own.
#ifdef __SWITCH__
#include "install/EngineStream.hpp"
#include "install/EngineHooks.hpp"
#include "core/NczPoint.hpp"
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <switch.h>
#include <threads.h>
#include "data/buffered_placeholder_writer.hpp"
#include "util/error.hpp"
#include "util/debug.h"
#include "util/util.hpp"
#include "util/lang.hpp"
#include "ui/instPage.hpp"
#include "ui/MainApplication.hpp"

namespace inst::ui { extern MainApplication *mainApp; }

namespace foil::install
{
namespace {
    std::string FormatOneDecimal(double value)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", value);
        return std::string(buf);
    }

    std::string FormatEta(std::uint64_t totalSeconds)
    {
        const std::uint64_t h = totalSeconds / 3600;
        const std::uint64_t m = (totalSeconds % 3600) / 60;
        const std::uint64_t s = totalSeconds % 60;
        char buf[32];
        if (h > 0) {
            std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                static_cast<unsigned long long>(h),
                static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(s));
        } else {
            std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                static_cast<unsigned long long>(m),
                static_cast<unsigned long long>(s));
        }
        return std::string(buf);
    }

// CookingFoil: atomic, read and written by three threads.
std::atomic<bool> stopThreadsHttpNsp{false};

struct RetryConfirmState
{
    std::atomic<bool> pending{false};
    std::atomic<bool> approved{false};
};

struct StreamFuncArgs
{
    tin::network::HTTPDownload* download;
    tin::data::BufferedPlaceholderWriter* bufferedPlaceholderWriter;
    u64 pfs0Offset;
    u64 ncaSize;
    RetryConfirmState retryConfirm;
};

// CookingFoil: the download and the writer (zstd, AES, ncm) each get a
// core of their own when the process may use it; by default every thread
// shares the drawing thread's core.
static void RunOnCore(int core)
{
    u64 mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && ((mask >> core) & 1))
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ULL << core);
}

// CookingFoil: the waits below sleep a millisecond instead of spinning;
// four spinning loops took the CPU the download and the writer need.
static constexpr u64 WaitNs = 1000 * 1000ULL;
static constexpr u64 ProgressNs = 20 * 1000 * 1000ULL;

int CurlStreamFunc(void* in)
{
    RunOnCore(2);
    StreamFuncArgs* args = reinterpret_cast<StreamFuncArgs*>(in);
    try {
        auto streamFunc = [&](u8* streamBuf, size_t streamBufSize) -> size_t
        {
            if (inst::ui::instPage::isInstallCancelRequested())
                return 0;
            while (true)
            {
                if (inst::ui::instPage::isInstallCancelRequested())
                    return 0;
                // CookingFoil: the writer stopped (an error) and will not
                // empty the ring; waiting would hang the install for good.
                if (stopThreadsHttpNsp)
                    return 0;
                if (args->bufferedPlaceholderWriter->CanAppendData(streamBufSize))
                    break;
                svcSleepThread(WaitNs);
            }

            args->bufferedPlaceholderWriter->AppendData(streamBuf, streamBufSize);
            return streamBufSize;
        };

        auto retryConfirmFunc = [&]() -> bool {
            args->retryConfirm.pending.store(true);
            while (args->retryConfirm.pending.load() && !stopThreadsHttpNsp)
                svcSleepThread(50 * 1000 * 1000ULL);
            return !stopThreadsHttpNsp && args->retryConfirm.approved.load();
        };

        if (args->download->StreamDataRange(args->pfs0Offset, args->ncaSize, streamFunc, retryConfirmFunc) == 1)
            stopThreadsHttpNsp = true;
    }
    catch (...) {
        stopThreadsHttpNsp = true;
    }
    return 0;
}

int PlaceholderWriteFunc(void* in)
{
    RunOnCore(1);
    StreamFuncArgs* args = reinterpret_cast<StreamFuncArgs*>(in);
    try {
        while (!args->bufferedPlaceholderWriter->IsPlaceholderComplete() && !stopThreadsHttpNsp)
        {
            if (inst::ui::instPage::isInstallCancelRequested()) {
                stopThreadsHttpNsp = true;
                break;
            }
            if (args->bufferedPlaceholderWriter->CanWriteSegmentToPlaceholder())
                args->bufferedPlaceholderWriter->WriteSegmentToPlaceholder();
            else
                svcSleepThread(WaitNs);
        }
    }
    catch (...) {
        stopThreadsHttpNsp = true;
    }

    return 0;
}
} // namespace

void streamNcz(tin::network::HTTPDownload& download, std::shared_ptr<nx::ncm::ContentStorage>& contentStorage,
               NcmContentId placeholderId, const std::string& displayFileName, u64 ncaStart, size_t ncaSize)
{
    // CookingFoil: a solid NCZ keeps points as it goes, and one cut off
    // goes on from its last point: the decoder and the hash come back,
    // the stream is asked for from where the point stands. Anything off
    // about the point (or a failed restore) starts it over.
    const auto makeResume = [&] {
        auto resume = std::make_shared<NczResume>();
        resume->maxWindow = foil::install::nczMaxWindow();
        resume->keep = [name = displayFileName, ncaSize](const std::string& point, const std::vector<u8>& window, u64 produced) {
            foil::install::keepNczPoint(name, ncaSize, point, window, produced);
        };
        return resume;
    };
    u64 skip = 0;
    std::unique_ptr<tin::data::BufferedPlaceholderWriter> placeholderWriter;
    {
        std::string point;
        std::vector<uint8_t> window;
        foil::NczPoint at;
        if (foil::install::nczPoint(displayFileName, ncaSize, point, window) && foil::NczPoint::decode(point, at) &&
            contentStorage->HasPlaceholder(*(NcmPlaceHolderId*)&placeholderId))
        {
            try {
                auto resume = makeResume();
                resume->point = point;
                resume->window.assign(window.begin(), window.end());
                resume->prefix.resize(at.zstdStart);
                download.BufferDataRange(resume->prefix.data(), ncaStart, at.zstdStart, nullptr);
                skip = at.zstdStart + at.consumed;
                placeholderWriter = std::make_unique<tin::data::BufferedPlaceholderWriter>(contentStorage, placeholderId, ncaSize - skip, resume);
            } catch (...) {
                placeholderWriter.reset();
                skip = 0;
            }
        }
        if (!placeholderWriter)
        {
            // From the start: what an earlier try left goes, point and all.
            foil::install::forgetResume(displayFileName);
            if (contentStorage->HasPlaceholder(*(NcmPlaceHolderId*)&placeholderId))
                contentStorage->DeletePlaceholder(*(NcmPlaceHolderId*)&placeholderId);
            placeholderWriter = std::make_unique<tin::data::BufferedPlaceholderWriter>(contentStorage, placeholderId, ncaSize, makeResume());
        }
    }
    tin::data::BufferedPlaceholderWriter& bufferedPlaceholderWriter = *placeholderWriter;
    StreamFuncArgs args;
    args.download = &download;
    args.bufferedPlaceholderWriter = &bufferedPlaceholderWriter;
    args.pfs0Offset = ncaStart + skip;
    args.ncaSize = ncaSize - skip;
    thrd_t curlThread;
    thrd_t writeThread;

    stopThreadsHttpNsp = false;
    // CookingFoil: both threads use `args` and the writer on this stack,
    // so whatever leaves this function (an exception included) stops and
    // joins them first; a thread that could not start is never joined.
    struct Joiner {
        thrd_t* curl = nullptr;
        thrd_t* write = nullptr;
        ~Joiner() {
            stopThreadsHttpNsp = true;
            if (curl) thrd_join(*curl, NULL);
            if (write) thrd_join(*write, NULL);
        }
    } joiner;
    if (thrd_create(&curlThread, CurlStreamFunc, &args) != thrd_success)
        THROW_FORMAT(("inst.net.transfer_interput"_lang).c_str());
    joiner.curl = &curlThread;
    if (thrd_create(&writeThread, PlaceholderWriteFunc, &args) != thrd_success)
        THROW_FORMAT(("inst.net.transfer_interput"_lang).c_str());
    joiner.write = &writeThread;

    u64 freq = armGetSystemTickFreq();
    u64 startTime = armGetSystemTick();
    size_t startSizeBuffered = 0;
    double emaSpeed = 0.0;

    inst::ui::instPage::setInstBarPerc(0);
    while (!bufferedPlaceholderWriter.IsBufferDataComplete() && !stopThreadsHttpNsp)
    {
        svcSleepThread(ProgressNs);
        if (inst::ui::instPage::isInstallCancelRequested()) {
            args.retryConfirm.approved.store(false);
            args.retryConfirm.pending.store(false);
            stopThreadsHttpNsp = true;
            break;
        }

        if (args.retryConfirm.pending.load())
        {
            int choice = inst::ui::mainApp->CreateShowDialog(
                "inst.net.retry.title"_lang,
                "inst.net.retry.desc"_lang,
                {"inst.net.retry.yes"_lang, "inst.net.retry.no"_lang},
                true);
            args.retryConfirm.approved.store(choice == 0);
            args.retryConfirm.pending.store(false);
        }
        u64 newTime = armGetSystemTick();

        if (newTime - startTime >= freq * 0.5)
        {
            size_t newSizeBuffered = bufferedPlaceholderWriter.GetSizeBuffered();
            double mbBuffered = double(newSizeBuffered - startSizeBuffered) / (1024 * 1024);
            double duration = ((double)(newTime - startTime) / (double)freq);
            double speed =  mbBuffered / duration;

            if (emaSpeed <= 0.0) {
                emaSpeed = speed;
            } else {
                emaSpeed = (emaSpeed * 0.7) + (speed * 0.3);
            }

            startTime = newTime;
            startSizeBuffered = newSizeBuffered;

            // CookingFoil: counted from the file's start when it went on from a point.
            const double sizeBuffered = static_cast<double>(bufferedPlaceholderWriter.GetSizeBuffered() + skip);
            const double totalSize = static_cast<double>(ncaSize);
            int downloadProgress = (int)((sizeBuffered / totalSize) * 100.0);

            std::string etaText = "--:--";
            if (emaSpeed > 0.0 && totalSize > sizeBuffered) {
                const double remainingMb = (totalSize - sizeBuffered) / (1024 * 1024);
                const double etaSecondsF = remainingMb / emaSpeed;
                if (etaSecondsF < 86400.0) {
                    etaText = FormatEta(static_cast<std::uint64_t>(etaSecondsF));
                }
            }

            foil::install::reportBytes(displayFileName,static_cast<uint64_t>(sizeBuffered),emaSpeed);
            inst::ui::instPage::setInstInfoText("inst.info_page.downloading"_lang + inst::util::formatUrlString(displayFileName) + "inst.info_page.at"_lang + FormatOneDecimal(emaSpeed) + " MB/s");
            inst::ui::instPage::setInstBarPerc((double)downloadProgress);
            inst::ui::instPage::setProgressDetailText(
                "Downloaded " + FormatOneDecimal(sizeBuffered / (1024 * 1024)) + " / " +
                FormatOneDecimal(totalSize / (1024 * 1024)) + " MB (" +
                std::to_string(downloadProgress) + "%) • ETA " + etaText
            );
        }
    }
    foil::install::reportBytes(displayFileName,bufferedPlaceholderWriter.GetSizeBuffered() + skip,emaSpeed);
    inst::ui::instPage::setInstBarPerc(100);
    inst::ui::instPage::setProgressDetailText("Downloaded 100% • Verifying and installing...");

    inst::ui::instPage::setInstInfoText("inst.info_page.top_info0"_lang + displayFileName + "...");
    inst::ui::instPage::setInstBarPerc(0);
    while (!bufferedPlaceholderWriter.IsPlaceholderComplete() && !stopThreadsHttpNsp)
    {
        svcSleepThread(ProgressNs);
        if (inst::ui::instPage::isInstallCancelRequested()) {
            args.retryConfirm.approved.store(false);
            args.retryConfirm.pending.store(false);
            stopThreadsHttpNsp = true;
            break;
        }
        int installProgress = (int)(((double)bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / (double)bufferedPlaceholderWriter.GetTotalDataSize()) * 100.0);

        inst::ui::instPage::setInstBarPerc((double)installProgress);
        inst::ui::instPage::setProgressDetailText(
            "Installing " + FormatOneDecimal((double)bufferedPlaceholderWriter.GetSizeWrittenToPlaceholder() / 1000000.0) + " / " +
            FormatOneDecimal((double)bufferedPlaceholderWriter.GetTotalDataSize() / 1000000.0) + " MB (" +
            std::to_string(installProgress) + "%)"
        );
    }
    inst::ui::instPage::setInstBarPerc(100);
    inst::ui::instPage::setProgressDetailText("Installing 100%");

    thrd_join(curlThread, NULL);
    thrd_join(writeThread, NULL);
    joiner.curl = joiner.write = nullptr;
    if (inst::ui::instPage::isInstallCancelRequested())
    {
        // Cancelled on purpose: nothing of it stays to go on from. Stopped
        // (the app closing): its last point stays for the next start.
        if (inst::ui::instPage::isInstallDiscardRequested())
            foil::install::forgetResume(displayFileName);
        try { bufferedPlaceholderWriter.close(); } catch (...) {}
        THROW_FORMAT("Installation canceled.");
    }
    if (stopThreadsHttpNsp)
    {
        // Cut off: its last point stays for the next try.
        try { bufferedPlaceholderWriter.close(); } catch (...) {}
        THROW_FORMAT(("inst.net.transfer_interput"_lang).c_str());
    }
    try {
        bufferedPlaceholderWriter.close();
    } catch (...) {
        // The resumed file did not hash to its id: next time, from the start.
        foil::install::forgetResume(displayFileName);
        throw;
    }
    foil::install::forgetResume(displayFileName); // done: the point is spent
}
} // namespace foil::install
#endif
