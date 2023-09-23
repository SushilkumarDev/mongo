/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/idl/idl_parser.h"

namespace mongo {


REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamOplogMatch,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamOplogMatch::createFromBson,
                                  true);

namespace change_stream_filter {
/**
 * Produce the MatchExpression representing the filter for the $match stage to filter oplog entries
 * to only those relevant for this $changeStream stage.
 *
 * If there is a 'userMatch' $match stage that will apply to documents generated by the
 * $changeStream, this filter can incorporate portions of the predicate, so as to filter out oplog
 * entries that would definitely be filtered out by the 'userMatch' filter.
 *
 * NB: When passing a non-NULL 'userMatch' expression, the resulting expression is built using a
 * "shallow clone" of the 'userMatch' (i.e., the result of 'MatchExpression::clone()') and
 * can contain references to strings in the BSONObj that 'userMatch' originated from. Callers that
 * keep the new filter long-term should serialize and re-parse it to guard against the possibility
 * of stale string references.
 */
std::unique_ptr<MatchExpression> buildOplogMatchFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp startFromInclusive,
    const MatchExpression* userMatch = nullptr) {
    tassert(6394401,
            "Expected changeStream spec to be present while building the oplog match filter",
            expCtx->changeStreamSpec);

    // Start building the oplog filter by adding predicates that apply to every entry.
    auto oplogFilter = std::make_unique<AndMatchExpression>();
    oplogFilter->add(buildTsFilter(expCtx, startFromInclusive, userMatch));
    if (!expCtx->changeStreamSpec->getShowMigrationEvents()) {
        oplogFilter->add(buildNotFromMigrateFilter(expCtx, userMatch));
    }

    // Create an $or filter which only captures relevant events in the oplog.
    auto eventFilter = std::make_unique<OrMatchExpression>();
    eventFilter->add(buildOperationFilter(expCtx, userMatch));
    eventFilter->add(buildInvalidationFilter(expCtx, userMatch));
    eventFilter->add(buildTransactionFilter(expCtx, userMatch));
    eventFilter->add(buildInternalOpFilter(expCtx, userMatch));

    // We currently do not support opening a change stream on a view namespace. So we only need to
    // add this filter when the change stream type is whole-db or whole cluster.
    if (expCtx->changeStreamSpec->getShowExpandedEvents() &&
        expCtx->ns.isCollectionlessAggregateNS()) {
        eventFilter->add(buildViewDefinitionEventFilter(expCtx, userMatch));
    }

    // Build the final $match filter to be applied to the oplog.
    oplogFilter->add(std::move(eventFilter));

    // Perform a final optimization pass on the complete filter before returning.
    return MatchExpression::optimize(std::move(oplogFilter));
}
}  // namespace change_stream_filter

DocumentSourceChangeStreamOplogMatch::DocumentSourceChangeStreamOplogMatch(
    Timestamp clusterTime, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceMatch(change_stream_filter::buildOplogMatchFilter(expCtx, clusterTime),
                          expCtx) {
    _clusterTime = clusterTime;
    expCtx->tailableMode = TailableModeEnum::kTailableAndAwaitData;
}

boost::intrusive_ptr<DocumentSourceChangeStreamOplogMatch>
DocumentSourceChangeStreamOplogMatch::create(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             const DocumentSourceChangeStreamSpec& spec) {
    auto resumeToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);
    return make_intrusive<DocumentSourceChangeStreamOplogMatch>(resumeToken.clusterTime, expCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceChangeStreamOplogMatch::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(5467600,
            "the match filter must be an expression in an object",
            elem.type() == BSONType::Object);
    auto parsedSpec = DocumentSourceChangeStreamOplogMatchSpec::parse(
        IDLParserContext("DocumentSourceChangeStreamOplogMatchSpec"), elem.Obj());

    // Note: raw new used here to access private constructor.
    return new DocumentSourceChangeStreamOplogMatch(parsedSpec.getFilter(), pExpCtx);
}

const char* DocumentSourceChangeStreamOplogMatch::getSourceName() const {
    // This is used in error reporting, particularly if we find this stage in a position other
    // than first, so report the name as $changeStream.
    return kStageName.rawData();
}

StageConstraints DocumentSourceChangeStreamOplogMatch::constraints(
    Pipeline::SplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kFirst,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.isIndependentOfAnyCollection =
        pExpCtx->ns.isCollectionlessAggregateNS() ? true : false;
    return constraints;
}

Pipeline::SourceContainer::iterator DocumentSourceChangeStreamOplogMatch::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    tassert(5687203, "Iterator mismatch during optimization", *itr == this);

    auto nextChangeStreamStageItr = std::next(itr);

    // It is not safe to combine any parts of a user $match with this stage when the $user match has
    // a non-simple collation, because this stage's MatchExpression always executes wtih the simple
    // collation.
    if (pExpCtx->getCollator()) {
        return nextChangeStreamStageItr;
    }

    // Seek to the stage that immediately follows the change streams stages.
    itr = std::find_if_not(itr, container->end(), [](const auto& stage) {
        return stage->constraints().isChangeStreamStage();
    });

    // Optimize the pipeline after this stage to merge $match stages and push them forward. Note, if
    // we have already performed the '_optimizedEndOfPipeline' step, we assume that we have also
    // applied any $match rewrites that would be available to this function and that no further
    // optimization is necessary. The optimizations in this function are not designed to be
    // attempted multiple times.
    if (_optimizedEndOfPipeline) {
        return itr;
    }

    itr = Pipeline::optimizeEndOfPipeline(std::prev(itr), container);
    _optimizedEndOfPipeline = true;

    if (itr == container->end()) {
        // This pipeline is just the change stream.
        return itr;
    }

    auto matchStage = dynamic_cast<DocumentSourceMatch*>(itr->get());
    if (!matchStage) {
        // This function only attempts to optimize a $match that immediately follows expanded
        // $changeStream stages. That does not apply here, and we resume optimization at the last
        // change stream stage, in case a "swap" optimization can apply between it and the stage
        // that follows it. For example, $project stages can swap in front of the last change stream
        // stages.
        return std::prev(itr);
    }

    tassert(5687204, "Attempt to rewrite an interalOplogMatch after deserialization", _clusterTime);

    // Recreate the change stream filter with additional predicates from the user's $match.
    auto filterWithUserPredicates = change_stream_filter::buildOplogMatchFilter(
        pExpCtx, *_clusterTime, matchStage->getMatchExpression());

    // Set the internal DocumentSourceMatch state to the new filter.
    rebuild(filterWithUserPredicates->serialize());

    // Continue optimization at the next change stream stage.
    return nextChangeStreamStageItr;
}

Value DocumentSourceChangeStreamOplogMatch::serialize(const SerializationOptions& opts) const {
    BSONObjBuilder builder;
    if (opts.verbosity) {
        BSONObjBuilder sub(builder.subobjStart(DocumentSourceChangeStream::kStageName));
        sub.append("stage"_sd, kStageName);
        sub.append(DocumentSourceChangeStreamOplogMatchSpec::kFilterFieldName,
                   getMatchExpression()->serialize(opts));
        sub.done();
    } else {
        BSONObjBuilder sub(builder.subobjStart(kStageName));
        if (opts.literalPolicy != LiteralSerializationPolicy::kUnchanged ||
            opts.transformIdentifiers) {
            sub.append(DocumentSourceChangeStreamOplogMatchSpec::kFilterFieldName,
                       getMatchExpression()->serialize(opts));
        } else {
            DocumentSourceChangeStreamOplogMatchSpec(getPredicate()).serialize(&sub);
        }
        sub.done();
    }
    return Value(builder.obj());
}

}  // namespace mongo
