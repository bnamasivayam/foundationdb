/*
 * ProxyTLogPushMessageSerializer.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2021 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbserver/ptxn/ProxyTLogPushMessageSerializer.h"

#include "fdbclient/FDBTypes.h"
#include "fdbserver/SpanContextMessage.h"
#include "fdbserver/ptxn/MessageTypes.h"
#include "flow/Knobs.h"
#include "flow/serialize.h"

namespace ptxn {

void ProxyTLogPushMessageSerializer::writeMessage(const MutationRef& mutation, const StorageTeamID& team) {
	writeTransactionInfo(team);
	writers[team].writeItem(SubsequenceMutationItem{ currentSubsequence++, mutation });
}

void ProxyTLogPushMessageSerializer::writeMessage(const StringRef& mutation, const StorageTeamID& team) {
	writeTransactionInfo(team);
	writers[team].writeItem(SubsequenceMutationItem{ currentSubsequence++, mutation });
}

void ProxyTLogPushMessageSerializer::addTransactionInfo(const SpanID& context) {
	TEST(!spanContext.isValid()); // addTransactionInfo with invalid SpanID
	spanContext = context;
	writtenTeams.clear();
}

bool ProxyTLogPushMessageSerializer::writeTransactionInfo(StorageTeamID team) {
	if (!FLOW_KNOBS->WRITE_TRACING_ENABLED || writtenTeams.count(team) != 0 || !spanContext.isValid()) {
		// TODO: add (logSystem->getTLogVersion() < TLogVersion::V6) as condition here
		return false;
	}

	TEST(true); // Wrote SpanContextMessage to a tlog group
	writtenTeams.insert(team);
	writers[team].writeItem(SubsequenceMutationItem{ currentSubsequence++, SpanContextMessage(spanContext) });
	return true;
}

void ProxyTLogPushMessageSerializer::writeMessage(const MutationRef& mutation, const std::set<StorageTeamID>& teams) {
	// Write transaction info first for all teams
	for (const auto& team : teams) {
		// Note the increasing subsequence numbers.
		writeTransactionInfo(team);
	}

	for (const auto& team : teams) {
		// this mutation shares the same subsequence number
		writers[team].writeItem(SubsequenceMutationItem{ currentSubsequence, mutation });
	}
	currentSubsequence++;
}

void ProxyTLogPushMessageSerializer::completeMessageWriting(const StorageTeamID& storageTeamID) {
	writers[storageTeamID].completeItemWriting();

	ProxyTLogMessageHeader header;
	header.numItems = writers[storageTeamID].getNumItems();
	header.length = writers[storageTeamID].getItemsBytes();

	writers[storageTeamID].writeHeader(header);
}

Standalone<StringRef> ProxyTLogPushMessageSerializer::getSerialized(const StorageTeamID& storageTeamID) {
	ASSERT(writers[storageTeamID].isWritingCompleted());
	return writers[storageTeamID].getSerialized();
}

std::unordered_map<StorageTeamID, Standalone<StringRef>> ProxyTLogPushMessageSerializer::getAllSerialized() {
	std::unordered_map<StorageTeamID, Standalone<StringRef>> results;
	for (const auto& [teamID, writer] : writers) {
		if (!writer.isWritingCompleted()) {
			completeMessageWriting(teamID);
		}
		auto pair = results.emplace(teamID, writer.getSerialized());
		ASSERT(pair.second);
	}
	return results;
}

bool proxyTLogPushMessageDeserializer(const Arena& arena,
                                      StringRef serialized,
                                      ProxyTLogMessageHeader& header,
                                      std::vector<SubsequenceMutationItem>& messages) {
	return headeredItemDeserializerBase(arena, serialized, header, messages);
}

} // namespace ptxn