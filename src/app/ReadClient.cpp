/*
 *
 *    Copyright (c) 2021 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *      This file defines the initiator side of a CHIP Read Interaction.
 *
 */

#include <app/AppBuildConfig.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>

namespace chip {
namespace app {

CHIP_ERROR ReadClient::Init(Messaging::ExchangeManager * apExchangeMgr, Callback * apCallback, InteractionType aInteractionType)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    // Error if already initialized.
    VerifyOrExit(IsFree(), err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(apExchangeMgr != nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpExchangeMgr == nullptr, err = CHIP_ERROR_INVALID_ARGUMENT);
    mpExchangeMgr              = apExchangeMgr;
    mpCallback                 = apCallback;
    mState                     = ClientState::Initialized;
    mMinIntervalFloorSeconds   = 0;
    mMaxIntervalCeilingSeconds = 0;
    mSubscriptionId            = 0;
    mInitialReport             = true;
    mInteractionType           = aInteractionType;
    AbortExistingExchangeContext();

exit:
    return err;
}

void ReadClient::Shutdown()
{
    AbortExistingExchangeContext();
    ShutdownInternal(CHIP_NO_ERROR);
}

void ReadClient::ShutdownInternal(CHIP_ERROR aError)
{
    if (mpCallback != nullptr)
    {
        if (aError != CHIP_NO_ERROR)
        {
            mpCallback->OnError(this, aError);
        }
        mpCallback->OnDone(this);
        mpCallback = nullptr;
    }
    if (IsSubscriptionType())
    {
        CancelLivenessCheckTimer();
    }
    mMinIntervalFloorSeconds   = 0;
    mMaxIntervalCeilingSeconds = 0;
    mSubscriptionId            = 0;
    mInteractionType           = InteractionType::Read;
    mpExchangeMgr              = nullptr;
    mpExchangeCtx              = nullptr;
    mInitialReport             = true;
    mPeerNodeId                = kUndefinedNodeId;
    mFabricIndex               = kUndefinedFabricIndex;
    MoveToState(ClientState::Uninitialized);
}

const char * ReadClient::GetStateStr() const
{
#if CHIP_DETAIL_LOGGING
    switch (mState)
    {
    case ClientState::Uninitialized:
        return "UNINIT";
    case ClientState::Initialized:
        return "INIT";
    case ClientState::AwaitingInitialReport:
        return "AwaitingInitialReport";
    case ClientState::AwaitingSubscribeResponse:
        return "AwaitingSubscribeResponse";
    case ClientState::SubscriptionActive:
        return "SubscriptionActive";
    }
#endif // CHIP_DETAIL_LOGGING
    return "N/A";
}

void ReadClient::MoveToState(const ClientState aTargetState)
{
    mState = aTargetState;
    ChipLogDetail(DataManagement, "Client[%u] moving to [%s]", InteractionModelEngine::GetInstance()->GetReadClientArrayIndex(this),
                  GetStateStr());
}

CHIP_ERROR ReadClient::SendReadRequest(ReadPrepareParams & aReadPrepareParams)
{
    // TODO: SendRequest parameter is too long, need to have the structure to represent it
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferHandle msgBuf;
    ChipLogDetail(DataManagement, "%s: Client[%u] [%5.5s]", __func__,
                  InteractionModelEngine::GetInstance()->GetReadClientArrayIndex(this), GetStateStr());
    VerifyOrExit(ClientState::Initialized == mState, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpCallback != nullptr, err = CHIP_ERROR_INCORRECT_STATE);

    // Discard any existing exchange context. Effectively we can only have one exchange per ReadClient
    // at any one time.
    AbortExistingExchangeContext();

    {
        System::PacketBufferTLVWriter writer;
        ReadRequestMessage::Builder request;

        msgBuf = System::PacketBufferHandle::New(kMaxSecureSduLengthBytes);
        VerifyOrExit(!msgBuf.IsNull(), err = CHIP_ERROR_NO_MEMORY);

        writer.Init(std::move(msgBuf));

        err = request.Init(&writer);
        SuccessOrExit(err);

        if (aReadPrepareParams.mEventPathParamsListSize != 0 && aReadPrepareParams.mpEventPathParamsList != nullptr)
        {
            EventPaths::Builder & eventPathListBuilder = request.CreateEventPathsBuilder();
            SuccessOrExit(err = eventPathListBuilder.GetError());
            err = GenerateEventPaths(eventPathListBuilder, aReadPrepareParams.mpEventPathParamsList,
                                     aReadPrepareParams.mEventPathParamsListSize);
            SuccessOrExit(err);
            if (aReadPrepareParams.mEventNumber != 0)
            {
                // EventNumber is optional
                request.EventNumber(aReadPrepareParams.mEventNumber);
            }
        }

        if (aReadPrepareParams.mAttributePathParamsListSize != 0 && aReadPrepareParams.mpAttributePathParamsList != nullptr)
        {
            AttributePathList::Builder attributePathListBuilder = request.CreateAttributePathListBuilder();
            SuccessOrExit(err = attributePathListBuilder.GetError());
            err = GenerateAttributePathList(attributePathListBuilder, aReadPrepareParams.mpAttributePathParamsList,
                                            aReadPrepareParams.mAttributePathParamsListSize);
            SuccessOrExit(err);
        }

        request.EndOfReadRequestMessage();
        SuccessOrExit(err = request.GetError());

        err = writer.Finalize(&msgBuf);
        SuccessOrExit(err);
    }

    mpExchangeCtx = mpExchangeMgr->NewContext(aReadPrepareParams.mSessionHandle, this);
    VerifyOrExit(mpExchangeCtx != nullptr, err = CHIP_ERROR_NO_MEMORY);
    mpExchangeCtx->SetResponseTimeout(aReadPrepareParams.mTimeout);

    err = mpExchangeCtx->SendMessage(Protocols::InteractionModel::MsgType::ReadRequest, std::move(msgBuf),
                                     Messaging::SendFlags(Messaging::SendMessageFlags::kExpectResponse));
    SuccessOrExit(err);

    mPeerNodeId  = aReadPrepareParams.mSessionHandle.GetPeerNodeId();
    mFabricIndex = aReadPrepareParams.mSessionHandle.GetFabricIndex();

    MoveToState(ClientState::AwaitingInitialReport);

exit:

    if (err != CHIP_NO_ERROR)
    {
        AbortExistingExchangeContext();
    }

    return err;
}

CHIP_ERROR ReadClient::SendStatusResponse(CHIP_ERROR aError)
{
    using Protocols::InteractionModel::Status;

    System::PacketBufferHandle msgBuf = System::PacketBufferHandle::New(kMaxSecureSduLengthBytes);
    VerifyOrReturnLogError(!msgBuf.IsNull(), CHIP_ERROR_NO_MEMORY);

    System::PacketBufferTLVWriter writer;
    writer.Init(std::move(msgBuf));

    StatusResponseMessage::Builder response;
    ReturnLogErrorOnFailure(response.Init(&writer));
    Status statusCode = Status::Success;
    if (aError != CHIP_NO_ERROR)
    {
        statusCode = Status::InvalidSubscription;
    }
    response.Status(statusCode);
    ReturnLogErrorOnFailure(response.GetError());
    ReturnLogErrorOnFailure(writer.Finalize(&msgBuf));
    VerifyOrReturnLogError(mpExchangeCtx != nullptr, CHIP_ERROR_INCORRECT_STATE);

    if (IsSubscriptionType())
    {
        if (IsAwaitingInitialReport())
        {
            MoveToState(ClientState::AwaitingSubscribeResponse);
        }
        else
        {
            RefreshLivenessCheckTimer();
        }
    }
    ReturnLogErrorOnFailure(
        mpExchangeCtx->SendMessage(Protocols::InteractionModel::MsgType::StatusResponse, std::move(msgBuf),
                                   Messaging::SendFlags(IsAwaitingSubscribeResponse() ? Messaging::SendMessageFlags::kExpectResponse
                                                                                      : Messaging::SendMessageFlags::kNone)));
    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadClient::GenerateEventPaths(EventPaths::Builder & aEventPathsBuilder, EventPathParams * apEventPathParamsList,
                                          size_t aEventPathParamsListSize)
{
    CHIP_ERROR err = CHIP_NO_ERROR;

    for (size_t eventIndex = 0; eventIndex < aEventPathParamsListSize; ++eventIndex)
    {
        EventPathIB::Builder eventPathBuilder = aEventPathsBuilder.CreatePath();
        EventPathParams eventPath             = apEventPathParamsList[eventIndex];
        eventPathBuilder.Node(eventPath.mNodeId)
            .Event(eventPath.mEventId)
            .Endpoint(eventPath.mEndpointId)
            .Cluster(eventPath.mClusterId)
            .EndOfEventPathIB();
        SuccessOrExit(err = eventPathBuilder.GetError());
    }

    aEventPathsBuilder.EndOfEventPaths();
    SuccessOrExit(err = aEventPathsBuilder.GetError());

exit:
    return err;
}

CHIP_ERROR ReadClient::GenerateAttributePathList(AttributePathList::Builder & aAttributePathListBuilder,
                                                 AttributePathParams * apAttributePathParamsList,
                                                 size_t aAttributePathParamsListSize)
{
    for (size_t index = 0; index < aAttributePathParamsListSize; index++)
    {
        AttributePath::Builder attributePathBuilder = aAttributePathListBuilder.CreateAttributePathBuilder();
        attributePathBuilder.NodeId(apAttributePathParamsList[index].mNodeId)
            .EndpointId(apAttributePathParamsList[index].mEndpointId)
            .ClusterId(apAttributePathParamsList[index].mClusterId);
        if (apAttributePathParamsList[index].mFlags.Has(AttributePathParams::Flags::kFieldIdValid))
        {
            attributePathBuilder.FieldId(apAttributePathParamsList[index].mFieldId);
        }

        if (apAttributePathParamsList[index].mFlags.Has(AttributePathParams::Flags::kListIndexValid))
        {
            VerifyOrReturnError(apAttributePathParamsList[index].mFlags.Has(AttributePathParams::Flags::kFieldIdValid),
                                CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);
            attributePathBuilder.ListIndex(apAttributePathParamsList[index].mListIndex);
        }

        attributePathBuilder.EndOfAttributePath();
        ReturnErrorOnFailure(attributePathBuilder.GetError());
    }
    aAttributePathListBuilder.EndOfAttributePathList();
    return aAttributePathListBuilder.GetError();
}

CHIP_ERROR ReadClient::OnMessageReceived(Messaging::ExchangeContext * apExchangeContext, const PayloadHeader & aPayloadHeader,
                                         System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    VerifyOrExit(!IsFree(), err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpCallback != nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    if (aPayloadHeader.HasMessageType(Protocols::InteractionModel::MsgType::ReportData))
    {
        err = ProcessReportData(std::move(aPayload));
        SuccessOrExit(err);
    }
    else if (aPayloadHeader.HasMessageType(Protocols::InteractionModel::MsgType::SubscribeResponse))
    {
        VerifyOrExit(apExchangeContext == mpExchangeCtx, err = CHIP_ERROR_INCORRECT_STATE);
        err = ProcessSubscribeResponse(std::move(aPayload));
        // Forget the context as SUBSCRIBE RESPONSE is the last message in SUBSCRIBE transaction and
        // ExchangeContext::HandleMessage automatically closes a context if no other messages need to
        // be sent or received.
        mpExchangeCtx = nullptr;
        SuccessOrExit(err);
    }
    else
    {
        err = CHIP_ERROR_INVALID_MESSAGE_TYPE;
    }

exit:
    if (!IsSubscriptionType() || err != CHIP_NO_ERROR)
    {
        ShutdownInternal(err);
    }
    return err;
}

CHIP_ERROR ReadClient::AbortExistingExchangeContext()
{
    if (mpExchangeCtx != nullptr)
    {
        mpExchangeCtx->Abort();
        mpExchangeCtx = nullptr;
    }

    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadClient::OnUnsolicitedReportData(Messaging::ExchangeContext * apExchangeContext,
                                               System::PacketBufferHandle && aPayload)
{
    mpExchangeCtx  = apExchangeContext;
    CHIP_ERROR err = ProcessReportData(std::move(aPayload));
    mpExchangeCtx  = nullptr;
    if (err != CHIP_NO_ERROR)
    {
        ShutdownInternal(err);
    }
    return err;
}

CHIP_ERROR ReadClient::ProcessReportData(System::PacketBufferHandle && aPayload)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    ReportDataMessage::Parser report;

    bool isEventReportsPresent      = false;
    bool isAttributeDataListPresent = false;
    bool suppressResponse           = false;
    bool moreChunkedMessages        = false;
    uint64_t subscriptionId         = 0;
    EventReports::Parser EventReports;
    AttributeDataList::Parser attributeDataList;
    System::PacketBufferTLVReader reader;

    reader.Init(std::move(aPayload));
    reader.Next();

    err = report.Init(reader);
    SuccessOrExit(err);

#if CHIP_CONFIG_IM_ENABLE_SCHEMA_CHECK
    err = report.CheckSchemaValidity();
    SuccessOrExit(err);
#endif

    err = report.GetSuppressResponse(&suppressResponse);
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    err = report.GetSubscriptionId(&subscriptionId);
    if (CHIP_NO_ERROR == err)
    {
        if (IsInitialReport())
        {
            mSubscriptionId = subscriptionId;
        }
        else if (!IsMatchingClient(subscriptionId))
        {
            err = CHIP_ERROR_INVALID_ARGUMENT;
        }
    }
    else if (CHIP_END_OF_TLV == err)
    {
        if (IsSubscriptionType())
        {
            err = CHIP_ERROR_INVALID_ARGUMENT;
        }
        else
        {
            err = CHIP_NO_ERROR;
        }
    }
    SuccessOrExit(err);

    err = report.GetMoreChunkedMessages(&moreChunkedMessages);
    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    err                   = report.GetEventReports(&EventReports);
    isEventReportsPresent = (err == CHIP_NO_ERROR);
    if (err == CHIP_END_OF_TLV)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);

    if (isEventReportsPresent && nullptr != mpCallback)
    {
        chip::TLV::TLVReader EventReportsReader;
        EventReports.GetReader(&EventReportsReader);
        ChipLogProgress(DataManagement, "on event data is called!!!!!!!!!!!!!");
        mpCallback->OnEventData(this, EventReportsReader);
    }

    err                        = report.GetAttributeDataList(&attributeDataList);
    isAttributeDataListPresent = (err == CHIP_NO_ERROR);
    if (err == CHIP_END_OF_TLV)
    {
        err = CHIP_NO_ERROR;
    }
    SuccessOrExit(err);
    if (isAttributeDataListPresent && nullptr != mpCallback && !moreChunkedMessages)
    {
        chip::TLV::TLVReader attributeDataListReader;
        attributeDataList.GetReader(&attributeDataListReader);
        err = ProcessAttributeDataList(attributeDataListReader);
        SuccessOrExit(err);
    }

    if (!suppressResponse)
    {
        // TODO: Add status report support and correspond handler in ReadHandler, particular for situation when there
        // are multiple reports
    }

exit:
    SendStatusResponse(err);
    if (!mInitialReport)
    {
        mpExchangeCtx = nullptr;
    }
    mInitialReport = false;
    return err;
}

void ReadClient::OnResponseTimeout(Messaging::ExchangeContext * apExchangeContext)
{
    ChipLogProgress(DataManagement, "Time out! failed to receive report data from Exchange: " ChipLogFormatExchange,
                    ChipLogValueExchange(apExchangeContext));
    ShutdownInternal(CHIP_ERROR_TIMEOUT);
}

CHIP_ERROR ReadClient::ProcessAttributeDataList(TLV::TLVReader & aAttributeDataListReader)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    while (CHIP_NO_ERROR == (err = aAttributeDataListReader.Next()))
    {
        chip::TLV::TLVReader dataReader;
        AttributeDataElement::Parser element;
        AttributePath::Parser attributePathParser;
        ClusterInfo clusterInfo;
        uint16_t statusU16 = 0;
        auto status        = Protocols::InteractionModel::Status::Success;

        TLV::TLVReader reader = aAttributeDataListReader;
        err                   = element.Init(reader);
        SuccessOrExit(err);

        err = element.GetAttributePath(&attributePathParser);
        VerifyOrExit(err == CHIP_NO_ERROR, err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        // We are using the feature that the parser won't touch the value if the field does not exist, since all fields in the
        // cluster info will be invalid / wildcard, it is safe ignore CHIP_END_OF_TLV directly.

        err = attributePathParser.GetNodeId(&(clusterInfo.mNodeId));
        if (err == CHIP_END_OF_TLV)
        {
            err = CHIP_NO_ERROR;
        }
        VerifyOrExit(err == CHIP_NO_ERROR, err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        // The ReportData must contain a concrete attribute path

        err = attributePathParser.GetEndpointId(&(clusterInfo.mEndpointId));
        VerifyOrExit(err == CHIP_NO_ERROR, err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        err = attributePathParser.GetClusterId(&(clusterInfo.mClusterId));
        VerifyOrExit(err == CHIP_NO_ERROR, err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        err = attributePathParser.GetFieldId(&(clusterInfo.mFieldId));
        VerifyOrExit(err == CHIP_NO_ERROR, err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        err = attributePathParser.GetListIndex(&(clusterInfo.mListIndex));
        if (CHIP_END_OF_TLV == err)
        {
            err = CHIP_NO_ERROR;
        }
        VerifyOrExit(err == CHIP_NO_ERROR, err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        VerifyOrExit(clusterInfo.IsValidAttributePath(), err = CHIP_ERROR_IM_MALFORMED_ATTRIBUTE_PATH);

        err = element.GetData(&dataReader);
        if (err == CHIP_END_OF_TLV)
        {
            // The spec requires that one of data or status code must exist, thus failure to read data and status code means we
            // received malformed data from server.
            SuccessOrExit(err = element.GetStatus(&statusU16));
            status = static_cast<Protocols::InteractionModel::Status>(statusU16);
        }
        else if (err != CHIP_NO_ERROR)
        {
            ExitNow();
        }
        mpCallback->OnAttributeData(
            this, ConcreteAttributePath(clusterInfo.mEndpointId, clusterInfo.mClusterId, clusterInfo.mFieldId),
            status == Protocols::InteractionModel::Status::Success ? &dataReader : nullptr, StatusIB(status));
    }

    if (CHIP_END_OF_TLV == err)
    {
        err = CHIP_NO_ERROR;
    }

exit:
    return err;
}

CHIP_ERROR ReadClient::RefreshLivenessCheckTimer()
{
    CancelLivenessCheckTimer();
    ChipLogProgress(DataManagement, "Refresh LivenessCheckTime with %d seconds", mMaxIntervalCeilingSeconds);
    CHIP_ERROR err = InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionManager()->SystemLayer()->StartTimer(
        System::Clock::Seconds16(mMaxIntervalCeilingSeconds), OnLivenessTimeoutCallback, this);

    if (err != CHIP_NO_ERROR)
    {
        ShutdownInternal(err);
    }
    return err;
}

void ReadClient::CancelLivenessCheckTimer()
{
    InteractionModelEngine::GetInstance()->GetExchangeManager()->GetSessionManager()->SystemLayer()->CancelTimer(
        OnLivenessTimeoutCallback, this);
}

void ReadClient::OnLivenessTimeoutCallback(System::Layer * apSystemLayer, void * apAppState)
{
    ReadClient * const client = reinterpret_cast<ReadClient *>(apAppState);
    if (client->IsFree())
    {
        ChipLogError(DataManagement,
                     "ReadClient::OnLivenessTimeoutCallback invoked on a free client! This is a bug in CHIP stack!");
        return;
    }

    ChipLogError(DataManagement, "Subscription Liveness timeout with peer node 0x%" PRIx64 ", shutting down ", client->mPeerNodeId);
    client->mpExchangeCtx = nullptr;
    // TODO: add a more specific error here for liveness timeout failure to distinguish between other classes of timeouts (i.e
    // response timeouts).
    client->ShutdownInternal(CHIP_ERROR_TIMEOUT);
}

CHIP_ERROR ReadClient::ProcessSubscribeResponse(System::PacketBufferHandle && aPayload)
{
    System::PacketBufferTLVReader reader;
    reader.Init(std::move(aPayload));
    ReturnLogErrorOnFailure(reader.Next());

    SubscribeResponseMessage::Parser subscribeResponse;
    ReturnLogErrorOnFailure(subscribeResponse.Init(reader));

#if CHIP_CONFIG_IM_ENABLE_SCHEMA_CHECK
    ReturnLogErrorOnFailure(subscribeResponse.CheckSchemaValidity());
#endif

    uint64_t subscriptionId = 0;
    ReturnLogErrorOnFailure(subscribeResponse.GetSubscriptionId(&subscriptionId));
    VerifyOrReturnLogError(IsMatchingClient(subscriptionId), CHIP_ERROR_INVALID_ARGUMENT);
    ReturnLogErrorOnFailure(subscribeResponse.GetMinIntervalFloorSeconds(&mMinIntervalFloorSeconds));
    ReturnLogErrorOnFailure(subscribeResponse.GetMaxIntervalCeilingSeconds(&mMaxIntervalCeilingSeconds));

    if (mpCallback != nullptr)
    {
        mpCallback->OnSubscriptionEstablished(this);
    }

    MoveToState(ClientState::SubscriptionActive);

    return CHIP_NO_ERROR;
}

CHIP_ERROR ReadClient::SendSubscribeRequest(ReadPrepareParams & aReadPrepareParams)
{
    CHIP_ERROR err = CHIP_NO_ERROR;
    System::PacketBufferHandle msgBuf;
    System::PacketBufferTLVWriter writer;
    SubscribeRequestMessage::Builder request;
    VerifyOrExit(ClientState::Initialized == mState, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpExchangeCtx == nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    VerifyOrExit(mpCallback != nullptr, err = CHIP_ERROR_INCORRECT_STATE);
    msgBuf = System::PacketBufferHandle::New(kMaxSecureSduLengthBytes);
    VerifyOrExit(!msgBuf.IsNull(), err = CHIP_ERROR_NO_MEMORY);

    writer.Init(std::move(msgBuf));

    err = request.Init(&writer);
    SuccessOrExit(err);

    if (aReadPrepareParams.mEventPathParamsListSize != 0 && aReadPrepareParams.mpEventPathParamsList != nullptr)
    {
        EventPaths::Builder & eventPathListBuilder = request.CreateEventPathsBuilder();
        SuccessOrExit(err = eventPathListBuilder.GetError());
        err = GenerateEventPaths(eventPathListBuilder, aReadPrepareParams.mpEventPathParamsList,
                                 aReadPrepareParams.mEventPathParamsListSize);
        SuccessOrExit(err);

        if (aReadPrepareParams.mEventNumber != 0)
        {
            // EventNumber is optional
            request.EventNumber(aReadPrepareParams.mEventNumber);
        }
    }

    if (aReadPrepareParams.mAttributePathParamsListSize != 0 && aReadPrepareParams.mpAttributePathParamsList != nullptr)
    {
        AttributePathList::Builder & attributePathListBuilder = request.CreateAttributePathListBuilder();
        SuccessOrExit(err = attributePathListBuilder.GetError());
        err = GenerateAttributePathList(attributePathListBuilder, aReadPrepareParams.mpAttributePathParamsList,
                                        aReadPrepareParams.mAttributePathParamsListSize);
        SuccessOrExit(err);
    }

    request.MinIntervalSeconds(aReadPrepareParams.mMinIntervalFloorSeconds)
        .MaxIntervalSeconds(aReadPrepareParams.mMaxIntervalCeilingSeconds)
        .KeepSubscriptions(aReadPrepareParams.mKeepSubscriptions)
        .EndOfSubscribeRequestMessage();
    SuccessOrExit(err = request.GetError());

    err = writer.Finalize(&msgBuf);
    SuccessOrExit(err);

    mpExchangeCtx = mpExchangeMgr->NewContext(aReadPrepareParams.mSessionHandle, this);
    VerifyOrExit(mpExchangeCtx != nullptr, err = CHIP_ERROR_NO_MEMORY);
    mpExchangeCtx->SetResponseTimeout(kImMessageTimeout);

    err = mpExchangeCtx->SendMessage(Protocols::InteractionModel::MsgType::SubscribeRequest, std::move(msgBuf),
                                     Messaging::SendFlags(Messaging::SendMessageFlags::kExpectResponse));
    SuccessOrExit(err);

    mPeerNodeId  = aReadPrepareParams.mSessionHandle.GetPeerNodeId();
    mFabricIndex = aReadPrepareParams.mSessionHandle.GetFabricIndex();
    MoveToState(ClientState::AwaitingInitialReport);

exit:
    if (err != CHIP_NO_ERROR)
    {
        AbortExistingExchangeContext();
    }
    if (err != CHIP_NO_ERROR)
    {
        Shutdown();
    }
    return err;
}

}; // namespace app
}; // namespace chip
