#include "application/profiler/profiler_open_orchestrator.h"
#include "application/profiler/platform_metadata_capture.h"

#include <string>
#include <utility>

#include "application/profiler/live_statistics_payload.h"
#include "core/stats/system_stats.h"
#include "core/util/base64.h"
#include "core/util/format.h"
#include "core/util/monotonic_time.h"
#include "net/bytebin.h"
#include "net/gzip.h"
#include "profiling_window.h"
#include "spark_constants.h"

namespace spark {

namespace {

std::int64_t nowMs()
{
    return monotonicUnixMillis();
}

}  // namespace

ProfilerOpenOrchestrator::ProfilerOpenOrchestrator(Profiler &profiler, StatisticsService &statistics,
                                                   std::string bds_executable_sha256, std::string bytebin_url,
                                                   std::string viewer_url, std::string bytesocks_host,
                                                   TrustedViewersState &trusted_viewers,
                                                   MainThreadDispatcher &dispatcher,
                                                   ProfileMetadataProvider &metadata_provider, ResultNotifier &notifier)
    : profiler_(profiler), statistics_(statistics), bds_executable_sha256_(std::move(bds_executable_sha256)),
      bytebin_url_(std::move(bytebin_url)), viewer_url_(std::move(viewer_url)),
      bytesocks_host_(std::move(bytesocks_host)), trusted_viewers_(trusted_viewers), dispatcher_(dispatcher),
      metadata_provider_(metadata_provider), notifier_(notifier),
      viewer_schedule_(profiling_window::windowAdjustmentMs())
{
    viewer_worker_ = std::make_unique<ViewerUpdateWorker>(
        [this](const ViewerUpdateWorker::WorkItem &work) { return executeViewerWork(work); },
        [mailbox = mailbox_](ViewerUpdateWorker::Completion completion) {
            std::scoped_lock lock(mailbox->mutex);
            mailbox->result = std::move(completion);
        });
}

ProfilerOpenOrchestrator::~ProfilerOpenOrchestrator()
{
    if (!shutdownUntil(std::chrono::steady_clock::now() + std::chrono::seconds(5))) {
        std::terminate();
    }
}

void ProfilerOpenOrchestrator::cmdOpen(CommandSender &sender, const Arguments &args)
{
    drainCompletions();
    reapUntil(std::chrono::steady_clock::now());
    if (socket_state_ == SocketState::Retiring) {
        sender.sendErrorMessage("previous live viewer still closing; retry");
        return;
    }
    if (viewerOpenPending()) {
        sender.sendMessage("A live viewer is already being opened.");
        return;
    }
    if (viewer_socket_ && viewer_socket_->isOpen()) {
        sender.sendMessage("A live viewer is already open.");
        return;
    }
    if (viewer_socket_) {
        close();
        if (viewer_socket_) {
            sender.sendErrorMessage("previous live viewer still closing; retry");
            return;
        }
    }
    if (!profiler_.running()) {
        sender.sendMessage("The profiler isn't running! Start it first with: {}/spark profiler start", kColorGray);
        return;
    }
    auto key_pair = Crypto::generateKeyPair();
    if (key_pair.public_key_x509.empty()) {
        sender.sendErrorMessage("Failed to generate cryptographic key pair for the live viewer.");
        return;
    }

    ViewerSocket::Config config;
    config.bytesocks_host = bytesocks_host_;
    config.bytebin_url = bytebin_url_;
    config.viewer_url = viewer_url_;
    config.user_agent = std::string("endstone-spark/") + kVersion;
    config.sampler_interval = 60;
    config.statistics_interval = 10;

    auto socket = std::make_shared<ViewerSocket>(std::move(config), std::move(key_pair));
    socket->setIsKeyTrustedCallback([this](const std::vector<std::uint8_t> &key) {
        std::string b64 = base64Encode(key.data(), key.size());
        return trusted_viewers_.contains(b64);
    });

    std::string comment;
    const auto comments = args.stringFlag("comment");
    if (!comments.empty()) {
        comment = comments.front();
    }
    ExportContext context = captureLiveContext(nowMs(), comment);
    if (!startViewerWorker()) {
        sender.sendErrorMessage("Failed to start the live viewer worker.");
        return;
    }
    open_comment_ = comment;
    viewer_socket_ = socket;
    socket_state_ = SocketState::Opening;
    if (!viewer_worker_->enqueueOpen(std::move(context), socket, sender.getName())) {
        close();
        open_comment_.clear();
        sender.sendErrorMessage("Failed to start the live viewer worker.");
        return;
    }
    sender.sendMessage("{}Opening the live viewer...{}", kColorGold, kColorGray);
}

void ProfilerOpenOrchestrator::onTick(const std::string &fallback_sender_name)
{
    drainCompletions();
    reapUntil(std::chrono::steady_clock::now());
    if (viewer_worker_ && viewer_worker_->consumeFailure()) {
        notifyBestEffort(viewer_sender_name_.empty() ? fallback_sender_name : viewer_sender_name_,
                         "Live viewer worker failed.");
        close();
    }
    if (!profiler_.running()) {
        return;
    }

    if (socket_state_ == SocketState::Open && viewer_socket_) {
        if (!viewer_socket_->tick()) {
            std::string diagnostic = viewer_socket_->takeDiagnostic();
            if (!diagnostic.empty()) {
                notifyBestEffort(viewer_sender_name_.empty() ? fallback_sender_name : viewer_sender_name_, diagnostic);
            }
            close();
        }
        else if (viewer_socket_->isOpen()) {
            const auto now = nowMs();
            const LiveViewerDue due = viewer_schedule_.due(now);
            if (due.statistics || due.sampler) {
                const bool available = viewer_worker_ && viewer_worker_->available();
                const std::uint64_t generation = viewer_worker_ ? viewer_worker_->generation() : 0;
                if (available) {
                    ExportContext context =
                        due.sampler ? captureLiveContext(now, open_comment_) : captureLiveStatisticsContext(now);
                    bool queued = false;
                    if (due.statistics && due.sampler) {
                        queued = viewer_worker_->enqueueCombined(std::move(context), viewer_socket_, generation);
                    }
                    else if (due.statistics) {
                        queued = viewer_worker_->enqueueStatistics(std::move(context), viewer_socket_, generation);
                    }
                    else {
                        queued = viewer_worker_->enqueueSampler(std::move(context), viewer_socket_, generation);
                    }
                    if (queued) {
                        viewer_schedule_.commit(now, due);
                    }
                }
            }
        }
    }
}

void ProfilerOpenOrchestrator::notifyBestEffort(const std::string &sender_name, const std::string &message) noexcept
{
    try {
        notifier_.notify(sender_name, message);
    }
    catch (...) {  // NOLINT(bugprone-empty-catch): viewer notifications are best effort.
    }
}

void ProfilerOpenOrchestrator::close()
{
    if (viewer_worker_) {
        viewer_worker_->invalidate();
    }
    if (viewer_socket_) {
        socket_state_ = SocketState::Retiring;
        viewer_socket_->requestStop();
    }
    viewer_schedule_.disarm();
    viewer_sender_name_.clear();
    open_comment_.clear();
    reapUntil(std::chrono::steady_clock::now());
}

void ProfilerOpenOrchestrator::shutdown()
{
    static_cast<void>(shutdownUntil(std::chrono::steady_clock::now() + std::chrono::seconds(5)));
}

void ProfilerOpenOrchestrator::requestStop()
{
    if (viewer_worker_) {
        viewer_worker_->requestStop();
    }
    if (viewer_socket_) {
        socket_state_ = SocketState::Retiring;
        viewer_socket_->requestStop();
    }
    viewer_schedule_.disarm();
}

bool ProfilerOpenOrchestrator::shutdownUntil(std::chrono::steady_clock::time_point deadline)
{
    requestStop();
    const bool worker_stopped = !viewer_worker_ || viewer_worker_->stopUntil(deadline);
    drainCompletions();
    const bool socket_stopped = reapUntil(deadline);
    return worker_stopped && socket_stopped;
}

bool ProfilerOpenOrchestrator::retireUntil(std::chrono::steady_clock::time_point deadline)
{
    close();
    const bool idle = !viewer_worker_ || viewer_worker_->quiesceUntil(deadline);
    drainCompletions();
    return reapUntil(deadline) && idle;
}

bool ProfilerOpenOrchestrator::reapUntil(std::chrono::steady_clock::time_point deadline)
{
    if (socket_state_ != SocketState::Retiring) {
        return true;
    }
    if (viewer_socket_ && !viewer_socket_->closeUntil(deadline)) {
        return false;
    }
    if (viewer_worker_ && !viewer_worker_->quiesceUntil(deadline)) {
        return false;
    }
    viewer_socket_.reset();
    socket_state_ = SocketState::Empty;
    return true;
}

void ProfilerOpenOrchestrator::setViewerSocketForTesting(std::shared_ptr<ViewerSocket> socket)
{
    viewer_socket_ = std::move(socket);
    socket_state_ = viewer_socket_ ? SocketState::Open : SocketState::Empty;
    viewer_sender_name_ = "Console";
}

bool ProfilerOpenOrchestrator::startViewerWorker()
{
    return viewer_worker_ && viewer_worker_->start();
}

void ProfilerOpenOrchestrator::stopViewerWorker()
{
    if (viewer_worker_) {
        viewer_worker_->stop();
    }
}

std::string ProfilerOpenOrchestrator::executeViewerWork(const ViewerUpdateWorker::WorkItem &work)
{
    if (work.cancellation.stopRequested() || !viewerGenerationCurrent(work.generation) || !profiler_.running()) {
        return {};
    }

    if (work.type == ViewerUpdateWorker::WorkType::Open) {
        ExportContext context = work.context;
        const ViewerSocket::UploadCallback upload = [this, &context, &work](const std::string &channel_info_proto) {
            if (work.cancellation.stopRequested() || !viewerGenerationCurrent(work.generation) ||
                !profiler_.running()) {
                return std::string();
            }
            context.socket_channel_info_proto = channel_info_proto;
            return uploadSamplerData(context, work.cancellation);
        };
        return viewer_open_fn_ ? viewer_open_fn_(*work.socket, upload) : work.socket->open(upload, work.cancellation);
    }

    if (work.type == ViewerUpdateWorker::WorkType::Statistics) {
        const LiveStatisticsPayload payload = buildLiveStatisticsPayload(work.context);
        if (!work.cancellation.stopRequested()) {
            work.socket->sendStatistics(payload.platform, payload.system, payload.metrics);
        }
        return {};
    }

    if (work.type == ViewerUpdateWorker::WorkType::Combined) {
        const LiveStatisticsPayload payload = buildLiveStatisticsPayload(work.context);
        if (!work.cancellation.stopRequested()) {
            work.socket->sendStatistics(payload.platform, payload.system, payload.metrics);
        }
    }

    std::string bytebin_key = uploadSamplerData(work.context, work.cancellation);
    if (!work.cancellation.stopRequested() && !bytebin_key.empty() && viewerGenerationCurrent(work.generation)) {
        work.socket->sendUpdate(bytebin_key);
    }
    return {};
}

void ProfilerOpenOrchestrator::drainCompletions()
{
    std::optional<ViewerUpdateWorker::Completion> completion;
    {
        std::scoped_lock lock(mailbox_->mutex);
        completion = std::move(mailbox_->result);
        mailbox_->result.reset();
    }
    if (completion) {
        completeViewerOpen(*completion);
    }
}

void ProfilerOpenOrchestrator::completeViewerOpen(const ViewerUpdateWorker::Completion &completion)
{
    const auto socket = completion.socket.lock();
    if (socket_state_ != SocketState::Opening || socket != viewer_socket_ || !viewer_worker_ ||
        !viewer_worker_->completeOpen(completion.generation)) {
        return;
    }
    if (completion.url.empty() || !socket || !socket->isOpen() || !profiler_.running()) {
        close();
        open_comment_.clear();
        notifyBestEffort(completion.sender_name, "Failed to open the live viewer. Check your network connection.");
        return;
    }
    socket_state_ = SocketState::Open;
    viewer_sender_name_ = completion.sender_name;
    viewer_schedule_.arm(nowMs());
    notifyBestEffort(completion.sender_name, "Live viewer opened! Open it at: " + completion.url);
    notifyBestEffort(
        completion.sender_name,
        "The viewer updates statistics every 10 seconds and sampler data every minute while the profiler is running.");
}

ExportContext ProfilerOpenOrchestrator::captureLiveContext(std::int64_t now_ms, const std::string &comment)
{
    ExportContext context;
    context.bds_executable_sha256 = bds_executable_sha256_;
    gatherPlatformServerMetadata(metadata_provider_, context, now_ms);
    if (native_plugin_sources_provider_) {
        context.native_plugin_sources = native_plugin_sources_provider_();
    }
    context.comment = comment;
    context.statistics = statistics_.snapshot();
    context.metrics = statistics_.metricsSnapshot();
    context.window_stats = statistics_.profileWindows(profiler_.startTimeMs(), now_ms);
    context.system_stats = spark::gatherSystemStats(".");
    metadata_provider_.gatherWorldMetadata(context.world, context.minecraft_version);
    if (ping_samples_provider_) {
        context.ping_samples = ping_samples_provider_();
    }
    if (network_snapshot_provider_) {
        context.net_snapshots = network_snapshot_provider_();
    }
    return context;
}

ExportContext ProfilerOpenOrchestrator::captureLiveStatisticsContext(std::int64_t now_ms)
{
    ExportContext context;
    context.bds_executable_sha256 = bds_executable_sha256_;
    gatherPlatformServerMetadata(metadata_provider_, context, now_ms);
    context.statistics = statistics_.snapshot();
    context.metrics = statistics_.metricsSnapshot();
    context.system_stats = spark::gatherSystemStats(".");
    if (ping_samples_provider_) {
        context.ping_samples = ping_samples_provider_();
    }
    if (network_snapshot_provider_) {
        context.net_snapshots = network_snapshot_provider_();
    }
    return context;
}

std::string ProfilerOpenOrchestrator::uploadSamplerData(const ExportContext &context,
                                                        const CancellationToken &cancellation)
{
    if (cancellation.stopRequested()) {
        return {};
    }
    const bool tracking_was_suppressed = profiler_.setCurrentThreadAllocationTrackingSuppressed(true);
    try {
        std::string body = buildLiveSamplerData(context);
        if (body.empty() || cancellation.stopRequested()) {
            profiler_.setCurrentThreadAllocationTrackingSuppressed(tracking_was_suppressed);
            return {};
        }
        std::string compressed = gzipCompress(body, cancellation);
        UploadResult result = uploadToBytebin(compressed, bytebin_url_, kSamplerContentType,
                                              std::string("endstone-spark/") + kVersion, cancellation);
        profiler_.setCurrentThreadAllocationTrackingSuppressed(tracking_was_suppressed);
        return result.ok ? result.key : std::string();
    }
    catch (...) {
        profiler_.setCurrentThreadAllocationTrackingSuppressed(tracking_was_suppressed);
        if (cancellation.stopRequested()) {
            return {};
        }
        throw;
    }
}

std::string ProfilerOpenOrchestrator::buildLiveSamplerData(const ExportContext &context)
{
    return profiler_.liveExport(context);
}

bool ProfilerOpenOrchestrator::viewerGenerationCurrent(std::uint64_t generation) const
{
    return viewer_worker_ && viewer_worker_->current(generation);
}

}  // namespace spark
