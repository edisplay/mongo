/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/mirror_maestro.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/client_out_of_line_executor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/mirroring_sampler.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/hello/topology_version_observer.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_controllers.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/synchronized_value.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

namespace {
constexpr auto kMirrorMaestroName = "MirrorMaestro"_sd;
constexpr auto kMirrorMaestroThreadPoolMaxThreads = 2ull;  // Just enough to allow concurrency
constexpr auto kMirrorMaestroConnPoolMinSize = 1ull;       // Always be able to mirror eventually

constexpr auto kMirroredReadsParamName = "mirrorReads"_sd;

constexpr auto kMirroredReadsSeenKey = "seen"_sd;
constexpr auto kMirroredReadsSentKey = "sent"_sd;
constexpr auto kMirroredReadsErroredDuringSendKey = "erroredDuringSend"_sd;
constexpr auto kMirroredReadsProcessedAsSecondaryKey = "processedAsSecondary"_sd;
constexpr auto kMirroredReadsResolvedKey = "resolved"_sd;
constexpr auto kMirroredReadsResolvedBreakdownKey = "resolvedBreakdown"_sd;
constexpr auto kMirroredReadsSucceededKey = "succeeded"_sd;
constexpr auto kMirroredReadsPendingKey = "pending"_sd;
constexpr auto kMirroredReadsScheduledKey = "scheduled"_sd;

MONGO_FAIL_POINT_DEFINE(mirrorMaestroExpectsResponse);
MONGO_FAIL_POINT_DEFINE(mirrorMaestroTracksPending);
MONGO_FAIL_POINT_DEFINE(skipRegisteringMirroredReadsTopologyObserverCallback);

using Tag = std::pair<std::string, std::string>;

class MirrorMaestroImpl {
public:
    /**
     * Make the TaskExecutor and initialize other components.
     */
    void init(ServiceContext* serviceContext);

    /**
     * Shutdown the TaskExecutor and cancel any outstanding work.
     */
    void shutdown();

    /**
     * Mirror only if this maestro has been initialized.
     */
    void tryMirror(const std::shared_ptr<CommandInvocation>& invocation);

    /**
     * Returns the list of hosts to send mirrored reads to for targeted mirroring.
     */
    std::vector<HostAndPort> getCachedHostsForTargetedMirroring();

    /**
     * Update the list of hosts to target for targeted mirroring. The list of hosts will be updated
     * iff the config version has been incremented, or the replica set tag being used to target
     * hosts has been changed.
     */
    void updateCachedHostsForTargetedMirroring(const repl::ReplSetConfig& replSetConfig,
                                               bool tagChanged);

    auto getExecutor() {
        return _executor;
    }

    /**
     * Maintains the state required for mirroring requests.
     */
    class MirroredRequestState {
    public:
        MirroredRequestState(MirrorMaestroImpl* maestro,
                             std::vector<HostAndPort> hosts,
                             std::shared_ptr<CommandInvocation> invocation,
                             MirroredReadsParameters params,
                             double mirrorCount)
            : _maestro(std::move(maestro)),
              _hosts(std::move(hosts)),
              _invocation(std::move(invocation)),
              _params(std::move(params)),
              _mirrorCount(mirrorCount) {}

        MirroredRequestState() = delete;

        void mirror() {
            invariant(_maestro);
            _maestro->_mirror(_hosts, *_invocation, _params, _mirrorCount);
        }

    private:
        MirrorMaestroImpl* _maestro;
        std::vector<HostAndPort> _hosts;
        std::shared_ptr<CommandInvocation> _invocation;
        MirroredReadsParameters _params;
        double _mirrorCount;
    };

    /**
     * Maintains the list of hosts to mirror to when targeted mirroring is enabled. Targeted
     * mirroring uses replica set tags in order to target specific nodes.
     */
    class TargetedHostsCacheManager {
    public:
        struct TaggedHostsType {
            repl::ConfigVersionAndTerm configVersionAndTerm;
            std::vector<HostAndPort> hosts;
        };

        using VersionedTaggedHostsType = VersionedValue<TaggedHostsType>;

        TargetedHostsCacheManager() = default;

        /**
         * Updates the list of hosts to send mirrored reads to for targeted mirroring. The hosts
         * should be updated upon an increment in config version, or if the user changes the replica
         * set tag that should be used to target nodes.
         */
        void maybeUpdateHosts(Tag tag, const repl::ReplSetConfig& replSetConfig, bool tagChanged) {
            std::lock_guard lk(_mutex);
            _hosts.refreshSnapshot(_taggedHostsSnapshot);

            if (MONGO_likely(_taggedHostsSnapshot)) {
                // The config version and term should never decrease.
                invariant(replSetConfig.getConfigVersionAndTerm() >=
                              _taggedHostsSnapshot->configVersionAndTerm,
                          "Unexpected stale config version");

                // If the version and term has not changed, and the replica set tag used to target
                // has not changed, do nothing.
                if (!tagChanged &&
                    replSetConfig.getConfigVersionAndTerm() ==
                        _taggedHostsSnapshot->configVersionAndTerm) {
                    return;
                }
            }

            auto updatedHosts = std::make_shared<TaggedHostsType>();
            auto& updatedHostsValue = *updatedHosts;

            updatedHostsValue.configVersionAndTerm = replSetConfig.getConfigVersionAndTerm();

            auto tagConfig = replSetConfig.getTagConfig();
            for (const auto& member : replSetConfig.members()) {
                for (auto&& it = member.tagsBegin(); it != member.tagsEnd(); ++it) {
                    if (tagConfig.getTagKey(*it) == tag.first &&
                        tagConfig.getTagValue(*it) == tag.second) {
                        updatedHostsValue.hosts.push_back(member.getHostAndPort());
                        break;
                    }
                }
            }

            _hosts.update(std::move(updatedHosts));
        }

        std::vector<HostAndPort> getHosts() {
            _hosts.refreshSnapshot(_taggedHostsSnapshot);
            if (MONGO_likely(_taggedHostsSnapshot)) {
                return _taggedHostsSnapshot->hosts;
            }

            return {};
        }


    private:
        // Mutex used only to serialize updates to _hosts
        stdx::mutex _mutex;
        static thread_local VersionedTaggedHostsType::Snapshot _taggedHostsSnapshot;
        VersionedTaggedHostsType _hosts;
    };

private:
    friend void updateCachedHostsForTargetedMirroring_forTest(
        ServiceContext* serviceContext, const repl::ReplSetConfig& replSetConfig, bool tagChanged);

    friend std::vector<HostAndPort> getCachedHostsForTargetedMirroring_forTest(
        ServiceContext* serviceContext);

    // TODO SERVER-104849 Update this function to refresh the snapshot of the server parameter
    // that will be stored on MirrorMaestroImpl
    /**
     * Returns the replica set tag that should be used to target mirrored reads.
     */
    Tag _getTagForTargetedMirror() const {
        auto tag = _params->_data.get().getTargetedMirroring().getTag();
        if (tag.isEmpty()) {
            return {"", ""};
        }

        auto tagElem = tag.firstElement();
        return {tagElem.fieldName(), tagElem.str()};
    }

    /**
     * Attempt to mirror invocation to a subset of hosts based on params
     *
     * This command is expected to only run on the _executor
     */
    void _mirror(const std::vector<HostAndPort>& hosts,
                 const CommandInvocation& invocation,
                 const MirroredReadsParameters& params,
                 double mirrorCount);

    /**
     * An enum detailing the liveness of the Maestro
     *
     * The state transition map for liveness looks like so:
     * kUninitialized -> kRunning, kShutdown
     * kRunning -> kShutdown
     * kShutdown -> null
     */
    enum class Liveness {
        kUninitialized,
        kRunning,
        kShutdown,
    };

    // InitializationGuard guards and serializes the initialization and shutdown of members
    struct InitializationGuard {
        stdx::mutex mutex;
        Liveness liveness;
    };
    InitializationGuard _initGuard;

    // _isInitialized guards the use of heap allocated members like _executor
    // Even if _isInitialized is true, any member function of the variables below must still be
    // inately thread safe. If _isInitialized is false, there may not even be correct pointers to
    // call member functions upon.
    AtomicWord<bool> _isInitialized;
    MirroredReadsServerParameter* _params = nullptr;
    MirroringSampler _sampler;
    std::shared_ptr<executor::TaskExecutor> _executor;
    repl::TopologyVersionObserver _topologyVersionObserver;
    synchronized_value<PseudoRandom> _random{PseudoRandom(SecureRandom{}.nextInt64())};
    TargetedHostsCacheManager _cachedHostsForTargetedMirrors;
};

thread_local MirrorMaestroImpl::TargetedHostsCacheManager::VersionedTaggedHostsType::Snapshot
    MirrorMaestroImpl::TargetedHostsCacheManager::_taggedHostsSnapshot;
const auto getMirrorMaestroImpl = ServiceContext::declareDecoration<MirrorMaestroImpl>();

// Define a new serverStatus section "mirroredReads"
class MirroredReadsSection final : public ServerStatusSection {
public:
    using CounterT = long long;

    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement&) const override {
        BSONObjBuilder section;
        section.append(kMirroredReadsSeenKey, seen.loadRelaxed());
        section.append(kMirroredReadsSentKey, sent.loadRelaxed());
        section.append(kMirroredReadsErroredDuringSendKey, erroredDuringSend.loadRelaxed());
        section.append(kMirroredReadsProcessedAsSecondaryKey, processedAsSecondary.loadRelaxed());

        if (MONGO_unlikely(mirrorMaestroExpectsResponse.shouldFail())) {
            // We only can see if the command resolved if we got a response
            section.append(kMirroredReadsResolvedKey, resolved.loadRelaxed());
            section.append(kMirroredReadsResolvedBreakdownKey, resolvedBreakdown.toBSON());
            section.append(kMirroredReadsSucceededKey, succeeded.loadRelaxed());
        }
        if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
            section.append(kMirroredReadsPendingKey, pending.loadRelaxed());
            section.append(kMirroredReadsScheduledKey, scheduled.loadRelaxed());
        }
        return section.obj();
    };

    /**
     * Maintains a breakdown for resolved requests by host name.
     * This class may only be used for testing (e.g., as part of a fail-point).
     */
    class ResolvedBreakdownByHost {
    public:
        void onResponseReceived(const HostAndPort& host) {
            const auto hostName = host.toString();
            stdx::lock_guard<stdx::mutex> lk(_mutex);

            if (_resolved.find(hostName) == _resolved.end()) {
                _resolved[hostName] = 0;
            }

            _resolved[hostName]++;
        }

        BSONObj toBSON() const {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            BSONObjBuilder bob;
            for (const auto& entry : _resolved) {
                bob.append(entry.first, entry.second);
            }
            return bob.obj();
        }

    private:
        mutable stdx::mutex _mutex;

        stdx::unordered_map<std::string, CounterT> _resolved;
    };

    ResolvedBreakdownByHost resolvedBreakdown;

    // Counts the number of operations (as primary) recognized as "to be mirrored".
    AtomicWord<CounterT> seen;
    // Counts the number of remote requests (for mirroring as primary) that have ever been
    // scheduled to be sent over the network.
    AtomicWord<CounterT> sent;
    // Counts the number of remote requests (as primary) that failed with some error when sending.
    AtomicWord<CounterT> erroredDuringSend;
    // Counts the number of responses (as primary) from secondaries after mirrored operations.
    AtomicWord<CounterT> resolved;
    // Counts the number of responses (as primary) of successful mirrored operations. Disabled by
    // default, hidden behind the mirrorMaestroExpectsResponse fail point.
    AtomicWord<CounterT> succeeded;
    // Counts the number of operations (as primary) that will be mirrored but are not yet scheduled.
    // Disabled by default, hidden behind the mirrorMaestroTracksPending fail point.
    AtomicWord<CounterT> pending;
    // Counts the number of operations (as primary) that are currently scheduled to be mirrored,
    // but have not yet received any response. Disabled by default, hidden behind the
    // mirrorMaestroTracksPending fail point.
    AtomicWord<CounterT> scheduled;
    // Counts the number of mirrored operations processed successfully by this node as a
    // secondary. Disabled by default, hidden behind the mirrorMaestroExpectsResponse fail point.
    AtomicWord<CounterT> processedAsSecondary;
};
auto& gMirroredReadsSection = *ServerStatusSectionBuilder<MirroredReadsSection>(
                                   std::string{MirrorMaestro::kServerStatusSectionName})
                                   .forShard();

auto parseMirroredReadsParameters(const BSONObj& obj) {
    IDLParserContext ctx("mirrorReads");
    return MirroredReadsParameters::parse(ctx, obj);
}

}  // namespace

void MirroredReadsServerParameter::append(OperationContext*,
                                          BSONObjBuilder* bob,
                                          StringData name,
                                          const boost::optional<TenantId>&) {
    auto subBob = BSONObjBuilder(bob->subobjStart(name));
    _data->serialize(&subBob);
}

Status MirroredReadsServerParameter::set(const BSONElement& value,
                                         const boost::optional<TenantId>&) try {
    auto obj = value.Obj();

    _data = parseMirroredReadsParameters(obj);

    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
}

Status MirroredReadsServerParameter::setFromString(StringData str,
                                                   const boost::optional<TenantId>&) try {
    auto obj = fromjson(str);

    _data = parseMirroredReadsParameters(obj);

    return Status::OK();
} catch (const AssertionException& e) {
    return e.toStatus();
}

void MirrorMaestro::init(ServiceContext* serviceContext) {
    auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
    invariant(replCoord);
    if (!replCoord->getSettings().isReplSet()) {
        // We only need a maestro if we're in a replica set
        return;
    }

    auto& impl = getMirrorMaestroImpl(serviceContext);
    impl.init(serviceContext);
}

void MirrorMaestro::shutdown(ServiceContext* serviceContext) {
    auto& impl = getMirrorMaestroImpl(serviceContext);
    impl.shutdown();
}

void MirrorMaestro::tryMirrorRequest(OperationContext* opCtx) {
    auto& impl = getMirrorMaestroImpl(opCtx->getServiceContext());

    const auto& invocation = CommandInvocation::get(opCtx);

    impl.tryMirror(invocation);
}

void MirrorMaestro::onReceiveMirroredRead(OperationContext* opCtx) {
    const auto& invocation = CommandInvocation::get(opCtx);
    if (MONGO_unlikely(invocation->isMirrored())) {
        gMirroredReadsSection.processedAsSecondary.fetchAndAddRelaxed(1);
    }
}

std::vector<HostAndPort> MirrorMaestroImpl::getCachedHostsForTargetedMirroring() {
    return _cachedHostsForTargetedMirrors.getHosts();
}

void MirrorMaestroImpl::updateCachedHostsForTargetedMirroring(
    const repl::ReplSetConfig& replSetConfig, bool tagChanged) {
    _cachedHostsForTargetedMirrors.maybeUpdateHosts(
        _getTagForTargetedMirror(), replSetConfig, tagChanged);
}

void MirrorMaestroImpl::tryMirror(const std::shared_ptr<CommandInvocation>& invocation) {
    if (!_isInitialized.load()) {
        // If we're not even available, nothing to do
        return;
    }

    invariant(invocation);
    if (!invocation->supportsReadMirroring()) {
        // That's all, folks
        return;
    }

    gMirroredReadsSection.seen.fetchAndAdd(1);

    auto params = _params->_data.get();
    if (params.getSamplingRate() == 0) {
        // Nothing to do if sampling rate is zero.
        return;
    }

    auto imr = _topologyVersionObserver.getCached();
    auto samplingParams = MirroringSampler::SamplingParameters(params.getSamplingRate());
    auto mirrorMode = _sampler.getMirrorMode(imr, samplingParams);
    if (!mirrorMode.shouldMirror()) {
        // If we wouldn't select a host, then nothing more to do
        return;
    }

    std::vector<HostAndPort> hostsForGeneralMirror;
    if (mirrorMode.generalEnabled) {
        hostsForGeneralMirror = _sampler.getRawMirroringTargetsForGeneralMode(imr);
        invariant(!hostsForGeneralMirror.empty());
    }

    auto clientExecutor = ClientOutOfLineExecutor::get(Client::getCurrent());
    auto clientExecutorHandle = clientExecutor->getHandle();

    // NOTE: before using Client's out-of-line executor outside of MirrorMaestro, we must first
    // move the consumption (i.e., `consumeAllTasks`) to the baton.
    clientExecutor->consumeAllTasks();

    auto mirrorCount = std::ceil(params.getSamplingRate() * hostsForGeneralMirror.size());

    if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
        gMirroredReadsSection.pending.fetchAndAdd(mirrorCount);
    }

    // There is the potential to actually mirror requests, so schedule the _mirror() invocation
    // out-of-line. This means the command itself can return quickly and we do the arduous work of
    // building new bsons and evaluating randomness in a less important context.
    auto requestState = std::make_unique<MirroredRequestState>(
        this, std::move(hostsForGeneralMirror), invocation, std::move(params), mirrorCount);
    ExecutorFuture(_executor)  //
        .getAsync([clientExecutorHandle,
                   requestState = std::move(requestState)](const auto& status) mutable {
            if (!ErrorCodes::isShutdownError(status)) {
                invariant(status);
                requestState->mirror();
            }
            clientExecutorHandle.schedule([requestState = std::move(requestState)](
                                              const Status&) mutable { requestState.reset(); });
        });
}

void MirrorMaestroImpl::_mirror(const std::vector<HostAndPort>& hosts,
                                const CommandInvocation& invocation,
                                const MirroredReadsParameters& params,
                                const double mirrorCount) try {
    auto payload = [&] {
        BSONObjBuilder bob;

        invocation.appendMirrorableRequest(&bob);

        // Limit the maxTimeMS
        bob.append("maxTimeMS", params.getMaxTimeMS());

        // Indicate that this is a mirrored read.
        bob.append("mirrored", true);

        {
            // Set secondaryPreferred read preference
            BSONObjBuilder rpBob = bob.subobjStart("$readPreference");
            rpBob.append("mode", "secondaryPreferred");
        }

        {
            // Set local read concern
            BSONObjBuilder rcBob = bob.subobjStart("readConcern");
            rcBob.append("level", "local");
        }
        return bob.obj();
    }();

    // Mirror to a normalized subset of eligible hosts (i.e., secondaries).
    const auto startIndex = (*_random)->nextInt64(hosts.size());

    for (auto i = 0; i < mirrorCount; i++) {
        auto& host = hosts[(startIndex + i) % hosts.size()];
        std::weak_ptr<executor::TaskExecutor> wExec(_executor);
        auto mirrorResponseCallback = [host, wExec = std::move(wExec)](auto& args) {
            if (!args.response.status.isOK()) {
                gMirroredReadsSection.erroredDuringSend.fetchAndAdd(1);
            }

            if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
                gMirroredReadsSection.scheduled.fetchAndSubtract(1);
            }

            if (MONGO_likely(!mirrorMaestroExpectsResponse.shouldFail())) {
                // If we don't expect responses, then there is nothing to do here
                return;
            }

            // Count both failed and successful reads as resolved
            gMirroredReadsSection.resolved.fetchAndAdd(1);
            gMirroredReadsSection.resolvedBreakdown.onResponseReceived(host);

            auto commandResultStatus = getStatusFromCommandResult(args.response.data);
            if (commandResultStatus.isOK()) {
                gMirroredReadsSection.succeeded.fetchAndAdd(1);
            }

            LOGV2_DEBUG(
                31457, 4, "Response received", "host"_attr = host, "response"_attr = args.response);

            if (ErrorCodes::isRetriableError(args.response.status)) {
                LOGV2_WARNING(5089200,
                              "Received mirroring response with a retriable failure",
                              "error"_attr = args.response);
                return;
            } else if (!args.response.isOK()) {
                if (args.response.status == ErrorCodes::CallbackCanceled) {
                    if (auto exec = wExec.lock(); exec && exec->isShuttingDown()) {
                        // The mirroring command was canceled as part of the executor being
                        // shutdown.
                        LOGV2_INFO(
                            7558901,
                            "Mirroring command callback was canceled due to maestro shutdown",
                            "error"_attr = args.response,
                            "host"_attr = host.toString());
                        return;
                    }
                }
                LOGV2_ERROR(4717301,
                            "Received mirroring response with a non-okay status",
                            "error"_attr = args.response,
                            "host"_attr = host.toString());
            }
        };

        auto newRequest = executor::RemoteCommandRequest(
            host, invocation.getDBForReadMirroring(), payload, nullptr /* opCtx */);

        newRequest.fireAndForget = true;
        if (MONGO_unlikely(mirrorMaestroExpectsResponse.shouldFail()))
            newRequest.fireAndForget = false;

        LOGV2_DEBUG(31455, 4, "About to mirror", "host"_attr = host, "request"_attr = newRequest);

        auto status =
            _executor->scheduleRemoteCommand(newRequest, std::move(mirrorResponseCallback))
                .getStatus();

        if (ErrorCodes::isShutdownError(status.code())) {
            LOGV2_DEBUG(5723501, 1, "Aborted mirroring due to shutdown", "reason"_attr = status);
            return;
        }

        tassert(status);
        if (MONGO_unlikely(mirrorMaestroTracksPending.shouldFail())) {
            // We've scheduled the operation to be mirrored; it is no longer "pending" and is now
            // "scheduled" until it has actually been resolved.
            gMirroredReadsSection.scheduled.fetchAndAdd(1);
            gMirroredReadsSection.pending.fetchAndSubtract(1);
        }
        gMirroredReadsSection.sent.fetchAndAdd(1);
    }
} catch (const DBException& e) {
    LOGV2_DEBUG(31456, 2, "Mirroring failed", "reason"_attr = e);
}

void MirrorMaestroImpl::init(ServiceContext* serviceContext) {
    LOGV2_DEBUG(31452, 2, "Initializing MirrorMaestro");

    // Until the end of this scope, no other thread can mutate _initGuard.liveness, so no other
    // thread can be in the critical section of init() or shutdown().
    stdx::lock_guard lk(_initGuard.mutex);
    switch (_initGuard.liveness) {
        case Liveness::kUninitialized: {
            // We can init
        } break;
        case Liveness::kRunning: {
            // If someone else already initialized, do nothing
            return;
        } break;
        case Liveness::kShutdown: {
            LOGV2_DEBUG(31453, 2, "Cannot initialize an already shutdown MirrorMaestro");
            return;
        } break;
    };

    auto makeNet = [&] {
        executor::ConnectionPool::Options options;
        options.controllerFactory = [] {
            return std::make_shared<executor::DynamicLimitController>(
                [] { return kMirrorMaestroConnPoolMinSize; },
                [] { return gMirrorMaestroConnPoolMaxSize.load(); },
                "MirrorMaestroDynamicLimitController");
        };
        return executor::makeNetworkInterface(
            std::string{kMirrorMaestroName}, {}, {}, std::move(options));
    };

    auto makePool = [&] {
        ThreadPool::Options options;
        options.poolName = std::string{kMirrorMaestroName};
        options.maxThreads = kMirrorMaestroThreadPoolMaxThreads;
        return std::make_unique<ThreadPool>(std::move(options));
    };
    _executor = executor::ThreadPoolTaskExecutor::create(makePool(), makeNet());

    _executor->startup();
    _topologyVersionObserver.init(serviceContext);
    if (MONGO_likely(!skipRegisteringMirroredReadsTopologyObserverCallback.shouldFail())) {
        _topologyVersionObserver.registerTopologyChangeObserver(
            [this](const repl::ReplSetConfig& replSetConfig) {
                updateCachedHostsForTargetedMirroring(replSetConfig, false /* tagChanged */);
            });
    }

    _params = ServerParameterSet::getNodeParameterSet()->get<MirroredReadsServerParameter>(
        kMirroredReadsParamName);
    invariant(_params);

    // Set _initGuard.liveness to kRunning
    _initGuard.liveness = Liveness::kRunning;

    // Mark the maestro as initialized. It is now safe to call tryMirrorRequest(), use the
    // _executor, or otherwise rely on members to be alive and well.
    _isInitialized.store(true);
}

void MirrorMaestroImpl::shutdown() {
    LOGV2_DEBUG(31454, 2, "Shutting down MirrorMaestro");

    // Until the end of this scope, no other thread can mutate _initGuard.liveness, so no other
    // thread can be in the critical section of init() or shutdown().
    stdx::lock_guard lk(_initGuard.mutex);
    switch (_initGuard.liveness) {
        case Liveness::kUninitialized:
        case Liveness::kShutdown: {
            // If someone else already shutdown or we never init'd, do nothing
            return;
        } break;
        case Liveness::kRunning: {
            // Time to shut it all down
        } break;
    };

    _topologyVersionObserver.shutdown();

    if (_executor) {
        _executor->shutdown();
        _executor->join();
    }

    // Set _initGuard.liveness to kShutdown
    _initGuard.liveness = Liveness::kShutdown;
}

std::shared_ptr<executor::TaskExecutor> getMirroringTaskExecutor_forTest(
    ServiceContext* serviceContext) {
    auto& impl = getMirrorMaestroImpl(serviceContext);
    return impl.getExecutor();
}

std::vector<HostAndPort> getCachedHostsForTargetedMirroring_forTest(
    ServiceContext* serviceContext) {
    auto& impl = getMirrorMaestroImpl(serviceContext);
    return impl.getCachedHostsForTargetedMirroring();
}

void updateCachedHostsForTargetedMirroring_forTest(ServiceContext* serviceContext,
                                                   const repl::ReplSetConfig& replSetConfig,
                                                   bool tagChanged) {
    auto& impl = getMirrorMaestroImpl(serviceContext);
    impl.updateCachedHostsForTargetedMirroring(replSetConfig, tagChanged);
}

}  // namespace mongo
