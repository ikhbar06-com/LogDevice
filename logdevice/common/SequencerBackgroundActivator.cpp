/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#include "logdevice/common/SequencerBackgroundActivator.h"

#include "logdevice/common/AllSequencers.h"
#include "logdevice/common/EpochMetaDataUpdater.h"
#include "logdevice/common/EpochSequencer.h"
#include "logdevice/common/MetaDataLog.h"
#include "logdevice/common/MetaDataLogWriter.h"
#include "logdevice/common/NodeSetSelectorFactory.h"
#include "logdevice/common/Processor.h"
#include "logdevice/common/Worker.h"

namespace facebook { namespace logdevice {

void SequencerBackgroundActivator::checkWorkerAsserts() {
  Worker* w = Worker::onThisThread();
  ld_check(w);
  ld_check(w->worker_type_ == getWorkerType(w->processor_));
  ld_check(w->idx_.val() ==
           getThreadAffinity(w->processor_->getWorkerCount(w->worker_type_)));
  ld_check(w->sequencerBackgroundActivator());
  ld_check(w->sequencerBackgroundActivator().get() == this);
}

void SequencerBackgroundActivator::schedule(std::vector<logid_t> log_ids) {
  checkWorkerAsserts();
  uint64_t num_scheduled = 0;
  for (auto& log_id : log_ids) {
    // metadata log sequencers don't interact via EpochStore, hence using this
    // state machine to activate them won't work
    ld_check(!MetaDataLog::isMetaDataLog(log_id));
    auto res = queue_.insert(log_id.val());
    if (res.second) {
      // new element inserted
      ++num_scheduled;
    }
  }
  bumpScheduledStat(num_scheduled);
  maybeProcessQueue();
}

bool SequencerBackgroundActivator::processOneLog(logid_t log_id,
                                                 ResourceBudget::Token& token) {
  auto config = Worker::onThisThread()->getConfig();
  const auto& nodes_configuration =
      Worker::onThisThread()->getNodesConfiguration();

  auto& all_seq = Worker::onThisThread()->processor_->allSequencers();
  auto seq = all_seq.findSequencer(log_id);

  if (!seq) {
    // No sequencer for that log, we're done with this one
    return true;
  }

  if (seq->background_activation_token_.valid()) {
    // Something's already in flight for this log. Don't do anything for now.
    // We'll be notified and run the check again when it completes.
    return true;
  }

  const auto my_node_id = config->serverConfig()->getMyNodeID();
  const bool is_sequencer_node =
      nodes_configuration->getSequencerMembership()->isSequencingEnabled(
          my_node_id.index());

  seq->noteConfigurationChanged(config, is_sequencer_node);

  if (!is_sequencer_node) {
    // no need to check for reactivation and such. The sequencer should've been
    // deactivated by the call to noteConfigurationChanged() above
    return true;
  }

  int rv = reprovisionOrReactivateIfNeeded(log_id, seq);
  if (rv == -1) {
    if (err == E::UPTODATE) {
      // No updates needed.
      return true;
    }

    // Reprovisioning could not be started, but may still be necessary.
    bool should_retry = err == E::FAILED || err == E::NOBUFS ||
        err == E::TOOMANY || err == E::NOTCONN || err == E::ACCESS;
    if (err != E::INPROGRESS && err != E::NOSEQUENCER) {
      RATELIMIT_INFO(std::chrono::seconds(10),
                     2,
                     "Got %s when checking if log %lu needs a metadata "
                     "update. Will%s try again later.",
                     error_name(err),
                     log_id.val(),
                     should_retry ? "" : " not");
    }
    return !should_retry;
  }

  // Reprovisioning in flight, moving token into the sequencer.
  ld_check(!seq->background_activation_token_.valid());
  seq->background_activation_token_ = std::move(token);
  return true;
}

int SequencerBackgroundActivator::reprovisionOrReactivateIfNeeded(
    logid_t logid,
    std::shared_ptr<Sequencer> seq) {
  ld_check(!MetaDataLog::isMetaDataLog(logid));
  ld_check(seq != nullptr);
  auto& all_seq = Worker::onThisThread()->processor_->allSequencers();

  // Only do anything if the sequencer is active.
  // If sequencer is inactive (e.g. preempted or error), it'll reprovision
  // metadata on next activation.
  // If sequencer activation is in progress, the sequencer will trigger another
  // call to this method when activation completes.
  // Also check that sequencer has epoch metadata; it may seem redundant because
  // sequencer always has epoch metadata if it's active, but there's a race
  // condition - maybe we grabbed state, then reactivation happened, then we
  // grabbed metadata.
  auto state = seq->getState();
  auto epoch_metadata = seq->getCurrentMetaData();
  if (state != Sequencer::State::ACTIVE || epoch_metadata == nullptr) {
    err =
        state == Sequencer::State::ACTIVATING ? E::INPROGRESS : E::NOSEQUENCER;
    return -1;
  }
  if (epoch_metadata->isEmpty() || epoch_metadata->disabled()) {
    ld_check(false);
    err = E::INTERNAL;
    return -1;
  }

  auto config = Worker::onThisThread()->getConfig();

  const std::shared_ptr<LogsConfig::LogGroupNode> logcfg =
      config->getLogGroupByIDShared(logid);
  if (!logcfg) {
    // logid no longer in config
    err = E::NOTFOUND;
    return -1;
  }

  bool need_reactivation = false;
  bool need_epoch_metadata_update = false;
  std::unique_ptr<EpochMetaData> new_metadata;

  epoch_t current_epoch = epoch_metadata->h.epoch;
  ld_check(current_epoch != EPOCH_INVALID);

  if (current_epoch.val() >= EPOCH_MAX.val() - 2) {
    // Ran out of epoch numbers, can't reactivate.
    err = E::TOOBIG;
    return -1;
  }

  folly::Optional<EpochSequencerImmutableOptions> current_options =
      seq->getEpochSequencerOptions();
  if (!current_options.hasValue()) {
    err = E::NOSEQUENCER;
    return -1;
  }
  EpochSequencerImmutableOptions new_options(
      logcfg->attrs(), Worker::settings());

  if (new_options != current_options.value()) {
    need_reactivation = true;
    RATELIMIT_INFO(std::chrono::seconds(10),
                   10,
                   "Reactivating sequencer for log %lu epoch %u "
                   "because options changed from %s to %s.",
                   logid.val(),
                   current_epoch.val(),
                   current_options.value().toString().c_str(),
                   new_options.toString().c_str());
  }

  do { // while (false)
    if (!config->serverConfig()->sequencersProvisionEpochStore()) {
      break;
    }

    ld_check(epoch_metadata);
    if (!epoch_metadata->writtenInMetaDataLog()) {
      // We can't reprovision metadata before it's written into the metadata
      // log. After it's written, SequencerMetaDataLogManager will re-check
      // whether reprovisioning is needed.
      err = E::INPROGRESS;
      return -1;
    }

    bool use_new_storage_set_format =
        Worker::settings().epoch_metadata_use_new_storage_set_format;

    // Use the same logic for updating metadata as during sequencer activation.

    bool only_nodeset_params_changed;

    // Copy sequencer's metadata and increment epoch. The result should be
    // equal to the metadata in epoch store (unless this sequencer is preempted,
    // which we will notice thanks to acceptable_activation_epoch).
    new_metadata = std::make_unique<EpochMetaData>(*epoch_metadata);
    ld_check(epoch_metadata->h.epoch < EPOCH_MAX);
    ++new_metadata->h.epoch.val_;

    EpochMetaData::UpdateResult res = updateMetaDataIfNeeded(
        logid,
        new_metadata,
        *config,
        /* target_nodeset_size */ folly::none, // TODO (#37918513): use
        /* nodeset_seed */ folly::none,        // TODO (#37918513): use
        /* nodeset_selector */ nullptr,
        use_new_storage_set_format,
        /* provision_if_empty */ false,
        /* update_if_exists */ true,
        /* force_update */ false,
        &only_nodeset_params_changed);
    if (res == EpochMetaData::UpdateResult::FAILED) {
      RATELIMIT_ERROR(
          std::chrono::seconds(10),
          2,
          "Failed to consider updating epoch metadata for log %lu: %s",
          logid.val(),
          error_name(err));
      new_metadata = nullptr;
      // This is unexpected. Don't update metadata and don't retry.
      break;
    }
    if (res == EpochMetaData::UpdateResult::UNCHANGED) {
      // No update needed.
      new_metadata = nullptr;
      break;
    }

    ld_check(res == EpochMetaData::UpdateResult::UPDATED);
    ld_check(new_metadata != nullptr);
    need_epoch_metadata_update = true;

    if (!only_nodeset_params_changed) {
      need_reactivation = true;
      RATELIMIT_INFO(std::chrono::seconds(10),
                     10,
                     "Reactivating sequencer for log %lu epoch %u to update "
                     "epoch metadata from %s to %s",
                     logid.val(),
                     current_epoch.val(),
                     epoch_metadata->toString().c_str(),
                     new_metadata->toString().c_str());
    } else {
      RATELIMIT_INFO(std::chrono::seconds(10),
                     10,
                     "Updating nodeset params in epoch store for log %lu epoch "
                     "%u from %s to %s without changing nodeset.",
                     logid.val(),
                     current_epoch.val(),
                     epoch_metadata->nodeset_params.toString().c_str(),
                     new_metadata->nodeset_params.toString().c_str());
    }

    // Assert that the nodeset selector is satisfied with the new nodeset and
    // won't want to change it again right away. Otherwise we may get into an
    // infinite loop of nodeset updates.
    {
      auto another_metadata = std::make_unique<EpochMetaData>(*new_metadata);
      bool another_only_params;
      auto another_res = updateMetaDataIfNeeded(
          logid,
          another_metadata,
          *config,
          /* target_nodeset_size */ folly::none, // TODO (#37918513): use
          /* nodeset_seed */ folly::none,        // TODO (#37918513): use
          /* nodeset_selector */ nullptr,
          use_new_storage_set_format,
          false,
          true,
          false,
          &another_only_params);
      // The first check is redundant but provides a better error message.
      if (!ld_catch(
              another_res != EpochMetaData::UpdateResult::FAILED,
              "updateMetaDataIfNeeded() succeeded, then failed when called "
              "again. This should be impossible. Log: %lu, epoch: %u, old "
              "metadata: %s, new metadata: %s",
              logid.val(),
              current_epoch.val(),
              epoch_metadata->toString().c_str(),
              new_metadata->toString().c_str()) ||
          !ld_catch(another_res == EpochMetaData::UpdateResult::UNCHANGED,
                    "updateMetaDataIfNeeded() wants to update metadata twice "
                    "in a row. "
                    "This should be impossible. Log: %lu, epoch: %u, old "
                    "metadata: %s, "
                    "new metadata: %s, yet another metadata: %s, first result: "
                    "%d %d, "
                    "second result: %d %d",
                    logid.val(),
                    current_epoch.val(),
                    epoch_metadata->toString().c_str(),
                    new_metadata->toString().c_str(),
                    another_metadata->toString().c_str(),
                    (int)res,
                    (int)only_nodeset_params_changed,
                    (int)another_res,
                    (int)another_only_params)) {
        // Cancel the update and return success to prevent retrying.
        need_epoch_metadata_update = false;
        need_reactivation = false;
      }
    }
  } while (false);

  if (need_reactivation) {
    WORKER_STAT_INCR(sequencer_reactivations_for_metadata_update);
    int rv = all_seq.activateSequencer(
        logid,
        "background reconfiguration",
        [](const Sequencer&) { return true; },
        /* acceptable_activation_epoch */ epoch_t(current_epoch.val() + 1),
        /* check_metadata_log_before_provisioning */ false,
        std::shared_ptr<EpochMetaData>(std::move(new_metadata)));
    if (rv != 0) {
      ld_check_in(err,
                  ({E::NOTFOUND,
                    E::NOBUFS,
                    E::INPROGRESS,
                    E::FAILED,
                    E::TOOMANY,
                    E::SYSLIMIT}));
    }
    return rv;
  }

  if (need_epoch_metadata_update) {
    // Update the nodeset params in epoch store without reactivating sequencer.
    WORKER_STAT_INCR(metadata_updates_without_sequencer_reactivation);
    ld_check(new_metadata != nullptr);
    auto callback =
        [&all_seq,
         seq,
         current_epoch,
         new_params = new_metadata->nodeset_params](
            Status st,
            logid_t log_id,
            std::unique_ptr<const EpochMetaData> info,
            std::unique_ptr<EpochStore::MetaProperties> meta_props) {
          if (st == E::OK || st == E::UPTODATE) {
            if (!seq->setNodeSetParamsInCurrentEpoch(
                    current_epoch, new_params)) {
              RATELIMIT_INFO(std::chrono::seconds(10),
                             2,
                             "Lost the race when updating nodeset params for "
                             "log %lu epoch %u to %s. This should be rare.",
                             log_id.val(),
                             current_epoch.val(),
                             new_params.toString().c_str());
            }
          }

          if (st == E::ABORTED) {
            // Epoch didn't match. Our sequencer is preempted.
            ld_check(info != nullptr);
            ld_check(info->h.epoch != EPOCH_INVALID);
            all_seq.notePreemption(log_id,
                                   epoch_t(info->h.epoch.val() - 1),
                                   meta_props.get(),
                                   seq.get(),
                                   "updating nodeset params");
          }

          if (st != E::SHUTDOWN && st != E::FAILED) {
            SequencerBackgroundActivator::requestNotifyCompletion(
                Worker::onThisThread()->processor_, log_id, st);
          }
        };
    MetaDataTracer tracer(Worker::onThisThread()->processor_->getTraceLogger(),
                          logid,
                          MetaDataTracer::Action::UPDATE_NODESET_PARAMS);
    int rv = all_seq.getEpochStore().createOrUpdateMetaData(
        logid,
        std::make_shared<EpochMetaDataUpdateNodeSetParams>(
            epoch_t(current_epoch.val() + 1), new_metadata->nodeset_params),
        callback,
        std::move(tracer),
        EpochStore::WriteNodeID::KEEP_LAST);
    if (rv != 0) {
      RATELIMIT_ERROR(
          std::chrono::seconds(10),
          2,
          "Failed to update nodeset params for log %lu in epoch store '%s': %s",
          logid.val(),
          all_seq.getEpochStore().identify().c_str(),
          error_name(err));
      ld_check_in(err,
                  ({E::INTERNAL,
                    E::NOTCONN,
                    E::ACCESS,
                    E::SYSLIMIT,
                    E::NOTFOUND,
                    E::FAILED}));
      return -1;
    }
    return 0;
  }

  err = E::UPTODATE;
  return -1;
}

void SequencerBackgroundActivator::notifyCompletion(logid_t logid,
                                                    Status /* st */) {
  checkWorkerAsserts();
  if (MetaDataLog::isMetaDataLog(logid)) {
    // We don't reactivate metadata logs.
    return;
  }
  auto seq =
      Worker::onThisThread()->processor_->allSequencers().findSequencer(logid);
  if (!seq) {
    // we don't care about this activation
    return;
  }

  // If the operation that just completed was triggered by us, reclaim the
  // in-flight slot we assigned to it.
  bool had_token = seq->background_activation_token_.valid();
  if (had_token) {
    seq->background_activation_token_.release();
  }

  // Schedule a re-check for the log, in case config was updated while sequencer
  // activation was in flight. Re-checking is cheap when no changes are needed.
  auto inserted = queue_.insert(logid.val()).second;

  if (had_token && !inserted) {
    bumpCompletedStat();
  }
  if (!had_token && inserted) {
    bumpScheduledStat();
  }

  maybeProcessQueue();
}

void SequencerBackgroundActivator::maybeProcessQueue() {
  checkWorkerAsserts();
  deactivateQueueProcessingTimer();
  size_t limit =
      Worker::settings().max_sequencer_background_activations_in_flight;
  if (!budget_) {
    budget_ = std::make_unique<ResourceBudget>(limit);
  } else if (budget_->getLimit() != limit) {
    // in case the setting changed after initialization
    budget_->setLimit(limit);
  }

  std::chrono::steady_clock::time_point start_time =
      std::chrono::steady_clock::now();
  bool made_progress = false;

  while (!queue_.empty() && budget_->available() > 0) {
    // Limiting this loop to 2ms
    if (made_progress && usec_since(start_time) > 2000) {
      // This is taking a while, let's yield for a few milliseconds.
      activateQueueProcessingTimer(std::chrono::microseconds(5000));
      break;
    }
    made_progress = true;

    ld_check(!queue_.empty());
    auto it = queue_.begin();
    logid_t log_id(*it);

    auto token = budget_->acquireToken();
    ld_check(token.valid());
    if (processOneLog(log_id, token)) {
      queue_.erase(it);

      if (token) {
        // The token hasn't been moved into the sequencer, presumably because
        // nothing needs to be done for this log.
        token.release();
        // Since we're releasing the token, we have to bump the stat.
        bumpCompletedStat();
      }
    } else {
      // Failed processing a sequencer. There's no point in retrying
      // immediately, so instead do it on a timer.
      activateQueueProcessingTimer(folly::none);
      break;
    }
  }
}

void SequencerBackgroundActivator::activateQueueProcessingTimer(
    folly::Optional<std::chrono::microseconds> timeout) {
  Worker* w = Worker::onThisThread();
  ld_check(w);
  if (!retry_timer_.isAssigned()) {
    retry_timer_.assign([this] { maybeProcessQueue(); });
  }
  if (!timeout.hasValue()) {
    timeout = Worker::settings().sequencer_background_activation_retry_interval;
  }
  retry_timer_.activate(timeout.value(), &w->commonTimeouts());
}

void SequencerBackgroundActivator::deactivateQueueProcessingTimer() {
  retry_timer_.cancel();
}

void SequencerBackgroundActivator::bumpCompletedStat(uint64_t val) {
  WORKER_STAT_ADD(background_sequencer_reactivations_completed, val);
}

void SequencerBackgroundActivator::bumpScheduledStat(uint64_t val) {
  WORKER_STAT_ADD(background_sequencer_reactivations_scheduled, val);
}

namespace {

class SequencerBackgroundActivatorRequest : public Request {
 public:
  explicit SequencerBackgroundActivatorRequest(
      Processor* processor,
      std::function<void(SequencerBackgroundActivator&)> func)
      : Request(RequestType::SEQUENCER_BACKGROUND_ACTIVATOR),
        workerType_(SequencerBackgroundActivator::getWorkerType(processor)),
        func_(func) {}

  WorkerType getWorkerTypeAffinity() override {
    return workerType_;
  }
  int getThreadAffinity(int nthreads) override {
    return SequencerBackgroundActivator::getThreadAffinity(nthreads);
  }

  Execution execute() override {
    auto& act = Worker::onThisThread()->sequencerBackgroundActivator();
    if (!act) {
      act = std::make_unique<SequencerBackgroundActivator>();
    }
    func_(*act);
    return Execution::COMPLETE;
  }

  WorkerType workerType_;
  std::function<void(SequencerBackgroundActivator&)> func_;
};

} // namespace

void SequencerBackgroundActivator::requestSchedule(Processor* processor,
                                                   std::vector<logid_t> logs) {
  ld_check(!logs.empty());
  std::unique_ptr<Request> rq =
      std::make_unique<SequencerBackgroundActivatorRequest>(
          processor,
          [logs = std::move(logs)](SequencerBackgroundActivator& act) mutable {
            act.schedule(std::move(logs));
          });
  int rv = processor->postImportant(rq);
  ld_check(rv == 0 || err == E::SHUTDOWN);
}

void SequencerBackgroundActivator::requestNotifyCompletion(Processor* processor,
                                                           logid_t log,
                                                           Status st) {
  std::unique_ptr<Request> rq =
      std::make_unique<SequencerBackgroundActivatorRequest>(
          processor, [log, st](SequencerBackgroundActivator& act) {
            act.notifyCompletion(log, st);
          });
  int rv = processor->postImportant(rq);
  ld_check(rv == 0 || err == E::SHUTDOWN);
}

}} // namespace facebook::logdevice
