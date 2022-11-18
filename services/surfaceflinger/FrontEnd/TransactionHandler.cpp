/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// #define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "TransactionHandler"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <cutils/trace.h>
#include <utils/Log.h>

#include "TransactionHandler.h"

namespace android::surfaceflinger::frontend {

void TransactionHandler::queueTransaction(TransactionState&& state) {
    mLocklessTransactionQueue.push(std::move(state));
    mPendingTransactionCount.fetch_add(1);
    ATRACE_INT("TransactionQueue", static_cast<int>(mPendingTransactionCount.load()));
}

std::vector<TransactionState> TransactionHandler::flushTransactions() {
    while (!mLocklessTransactionQueue.isEmpty()) {
        auto maybeTransaction = mLocklessTransactionQueue.pop();
        if (!maybeTransaction.has_value()) {
            break;
        }
        auto transaction = maybeTransaction.value();
        mPendingTransactionQueues[transaction.applyToken].emplace(std::move(transaction));
    }

    // Collect transaction that are ready to be applied.
    std::vector<TransactionState> transactions;
    TransactionFlushState flushState;
    flushState.queueProcessTime = systemTime();
    // Transactions with a buffer pending on a barrier may be on a different applyToken
    // than the transaction which satisfies our barrier. In fact this is the exact use case
    // that the primitive is designed for. This means we may first process
    // the barrier dependent transaction, determine it ineligible to complete
    // and then satisfy in a later inner iteration of flushPendingTransactionQueues.
    // The barrier dependent transaction was eligible to be presented in this frame
    // but we would have prevented it without case. To fix this we continually
    // loop through flushPendingTransactionQueues until we perform an iteration
    // where the number of transactionsPendingBarrier doesn't change. This way
    // we can continue to resolve dependency chains of barriers as far as possible.
    int lastTransactionsPendingBarrier = 0;
    int transactionsPendingBarrier = 0;
    do {
        lastTransactionsPendingBarrier = transactionsPendingBarrier;
        // Collect transactions that are ready to be applied.
        transactionsPendingBarrier = flushPendingTransactionQueues(transactions, flushState);
    } while (lastTransactionsPendingBarrier != transactionsPendingBarrier);

    mPendingTransactionCount.fetch_sub(transactions.size());
    ATRACE_INT("TransactionQueue", static_cast<int>(mPendingTransactionCount.load()));
    return transactions;
}

TransactionHandler::TransactionReadiness TransactionHandler::applyFilters(
        TransactionFlushState& flushState) {
    auto ready = TransactionReadiness::Ready;
    for (auto& filter : mTransactionReadyFilters) {
        auto perFilterReady = filter(flushState);
        switch (perFilterReady) {
            case TransactionReadiness::NotReady:
            case TransactionReadiness::NotReadyBarrier:
                return perFilterReady;

            case TransactionReadiness::ReadyUnsignaled:
            case TransactionReadiness::ReadyUnsignaledSingle:
                // If one of the filters allows latching an unsignaled buffer, latch this ready
                // state.
                ready = perFilterReady;
                break;
            case TransactionReadiness::Ready:
                continue;
        }
    }
    return ready;
}

int TransactionHandler::flushPendingTransactionQueues(std::vector<TransactionState>& transactions,
                                                      TransactionFlushState& flushState) {
    int transactionsPendingBarrier = 0;
    auto it = mPendingTransactionQueues.begin();
    while (it != mPendingTransactionQueues.end()) {
        auto& queue = it->second;
        IBinder* queueToken = it->first.get();

        // if we have already flushed a transaction with an unsignaled buffer then stop queue
        // processing
        if (std::find(flushState.queuesWithUnsignaledBuffers.begin(),
                      flushState.queuesWithUnsignaledBuffers.end(),
                      queueToken) != flushState.queuesWithUnsignaledBuffers.end()) {
            continue;
        }

        while (!queue.empty()) {
            auto& transaction = queue.front();
            flushState.transaction = &transaction;
            auto ready = applyFilters(flushState);
            if (ready == TransactionReadiness::NotReadyBarrier) {
                transactionsPendingBarrier++;
                break;
            } else if (ready == TransactionReadiness::NotReady) {
                break;
            }

            // Transaction is ready move it from the pending queue.
            flushState.firstTransaction = false;
            removeFromStalledTransactions(transaction.id);
            transactions.emplace_back(std::move(transaction));
            queue.pop();

            // If the buffer is unsignaled, then we don't want to signal other transactions using
            // the buffer as a barrier.
            auto& readyToApplyTransaction = transactions.back();
            if (ready == TransactionReadiness::Ready) {
                readyToApplyTransaction.traverseStatesWithBuffers([&](const layer_state_t& state) {
                    const bool frameNumberChanged = state.bufferData->flags.test(
                            BufferData::BufferDataChange::frameNumberChanged);
                    if (frameNumberChanged) {
                        flushState.bufferLayersReadyToPresent
                                .emplace_or_replace(state.surface.get(),
                                                    state.bufferData->frameNumber);
                    } else {
                        // Barrier function only used for BBQ which always includes a frame number.
                        // This value only used for barrier logic.
                        flushState.bufferLayersReadyToPresent
                                .emplace_or_replace(state.surface.get(),
                                                    std::numeric_limits<uint64_t>::max());
                    }
                });
            } else if (ready == TransactionReadiness::ReadyUnsignaledSingle) {
                // Track queues with a flushed unsingaled buffer.
                flushState.queuesWithUnsignaledBuffers.emplace_back(queueToken);
                break;
            }
        }

        if (queue.empty()) {
            it = mPendingTransactionQueues.erase(it);
        } else {
            it = std::next(it, 1);
        }
    }
    return transactionsPendingBarrier;
}

void TransactionHandler::addTransactionReadyFilter(TransactionFilter&& filter) {
    mTransactionReadyFilters.emplace_back(std::move(filter));
}

bool TransactionHandler::hasPendingTransactions() {
    return !mPendingTransactionQueues.empty() || !mLocklessTransactionQueue.isEmpty();
}

void TransactionHandler::onTransactionQueueStalled(uint64_t transactionId,
                                                   sp<ITransactionCompletedListener>& listener,
                                                   const std::string& reason) {
    if (std::find(mStalledTransactions.begin(), mStalledTransactions.end(), transactionId) !=
        mStalledTransactions.end()) {
        return;
    }

    mStalledTransactions.push_back(transactionId);
    listener->onTransactionQueueStalled(String8(reason.c_str()));
}

void TransactionHandler::removeFromStalledTransactions(uint64_t id) {
    auto it = std::find(mStalledTransactions.begin(), mStalledTransactions.end(), id);
    if (it != mStalledTransactions.end()) {
        mStalledTransactions.erase(it);
    }
}
} // namespace android::surfaceflinger::frontend
