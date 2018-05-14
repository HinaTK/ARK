/*
* This source file is part of ArkGameFrame
* For the latest info, see https://github.com/ArkGame
*
* Copyright (c) 2013-2018 ArkGame authors.
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
*
*/

#include "AFCWebSocktServer.h"
#include <string.h>

#include <atomic>
#include <memory>

void AFCWebSocktServer::Update()
{
    ProcessMsgLogicThread();
}

int AFCWebSocktServer::Start(const unsigned int nMaxClient, const std::string& strAddrPort, const int nServerID, const int nThreadCount)
{
    std::string strHost;
    int port;
    SplitHostPort(strAddrPort, strHost, port);
    m_plistenThread->startListen(false, strHost, port, std::bind(&AFCWebSocktServer::OnAcceptConnectionInner, this, std::placeholders::_1));

    m_pServer->startWorkThread(nThreadCount);
    return 0;
}

void AFCWebSocktServer::OnAcceptConnectionInner(brynet::net::TcpSocket::PTR socket)
{
    m_pServer->addSession(std::move(socket),
                          brynet::net::AddSessionOption::WithEnterCallback(
                              [this](const brynet::net::TCPSession::PTR & session)
    {
        brynet::net::HttpService::setup(session, std::bind(&AFCWebSocktServer::OnHttpConnect, this, std::placeholders::_1));
    }
                          ),
    brynet::net::AddSessionOption::WithMaxRecvBufferSize(1024 * 1024));
}
void AFCWebSocktServer::OnHttpMessageCallBack(const brynet::net::HTTPParser & httpParser, const brynet::net::HttpSession::PTR & session)
{
    brynet::net::HttpResponse response;
    response.setBody("test");
    std::string result = response.getResult();
    session->send(result.c_str(), result.size(), [session]()
    {
        session->postShutdown();
    });
}

void AFCWebSocktServer::OnWebSockMessageCallBack(const brynet::net::HttpSession::PTR & httpSession,
        brynet::net::WebSocketFormat::WebSocketFrameType opcode,
        const std::string & payload)
{

    switch(opcode)
    {
    case brynet::net::WebSocketFormat::WebSocketFrameType::ERROR_FRAME:
        break;
    case brynet::net::WebSocketFormat::WebSocketFrameType::CONTINUATION_FRAME:
        break;
    case brynet::net::WebSocketFormat::WebSocketFrameType::TEXT_FRAME:
        break;
    case brynet::net::WebSocketFormat::WebSocketFrameType::BINARY_FRAME:
        break;
    case brynet::net::WebSocketFormat::WebSocketFrameType::CLOSE_FRAME:
        break;
    case brynet::net::WebSocketFormat::WebSocketFrameType::PING_FRAME:
        {
            auto frame = std::make_shared<std::string>();
            brynet::net::WebSocketFormat::wsFrameBuild(payload.c_str(),
                    payload.size(),
                    *frame,
                    brynet::net::WebSocketFormat::WebSocketFrameType::PONG_FRAME,
                    true,
                    false);
            httpSession->send(frame);
            return;
        }
        break;
    case brynet::net::WebSocketFormat::WebSocketFrameType::PONG_FRAME:
        break;
    default:
        break;
    }

    const auto ud = brynet::net::cast<brynet::net::TcpService::SESSION_TYPE>(httpSession->getUD());
    AFGUID xClient(0, *ud);

    AFScopeRdLock xGuard(mRWLock);

    auto xFind = mmObject.find(xClient);
    if(xFind == mmObject.end())
    {
        return ;
    }

    xFind->second->AddBuff(payload.c_str(), payload.size());
    DismantleNet(xFind->second);
}

void AFCWebSocktServer::OnHttpConnect(const brynet::net::HttpSession::PTR& httpSession)
{
    httpSession->setHttpCallback(std::bind(&AFCWebSocktServer::OnHttpMessageCallBack, this, std::placeholders::_1, std::placeholders::_2));
    httpSession->setWSCallback(std::bind(&AFCWebSocktServer::OnWebSockMessageCallBack, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
    httpSession->setCloseCallback(std::bind(&AFCWebSocktServer::OnHttpDisConnection, this, std::placeholders::_1));

    MsgFromBryHttpNetInfo* pMsg = new MsgFromBryHttpNetInfo(httpSession);
    pMsg->xClientID.nLow = nNextID++;
    httpSession->setUD(static_cast<int64_t>(pMsg->xClientID.nLow));
    pMsg->nType = CONNECTED;
    {
        AFScopeWrLock xGuard(mRWLock);

        BryHttpNetObject* pEntity = new BryHttpNetObject(this, pMsg->xClientID, httpSession);
        if(AddNetObject(pMsg->xClientID, pEntity))
        {
            pEntity->mqMsgFromNet.Push(pMsg);
        }
    }
}

void AFCWebSocktServer::OnHttpDisConnection(const brynet::net::HttpSession::PTR & httpSession)
{
    const auto ud = brynet::net::cast<int64_t>(httpSession->getUD());
    AFGUID xClient(0, 0);
    xClient.nLow = *ud;
    AFScopeWrLock xGuard(mRWLock);

    auto xFind = mmObject.find(xClient);
    if(xFind == mmObject.end())
    {
        return ;
    }

    MsgFromBryHttpNetInfo* pMsg = new MsgFromBryHttpNetInfo(httpSession);
    pMsg->xClientID = xClient;
    pMsg->nType = DISCONNECTED;

    xFind->second->mqMsgFromNet.Push(pMsg);
}

bool AFCWebSocktServer::SplitHostPort(const std::string& strIpPort, std::string& host, int& port)
{
    std::string a = strIpPort;
    if(a.empty())
    {
        return false;
    }

    size_t index = a.rfind(':');
    if(index == std::string::npos)
    {
        return false;
    }

    if(index == a.size() - 1)
    {
        return false;
    }

    port = std::atoi(&a[index + 1]);

    host = std::string(strIpPort, 0, index);
    if(host[0] == '[')
    {
        if(*host.rbegin() != ']')
        {
            return false;
        }

        // trim the leading '[' and trail ']'
        host = std::string(host.data() + 1, host.size() - 2);
    }

    // Compatible with "fe80::886a:49f3:20f3:add2]:80"
    if(*host.rbegin() == ']')
    {
        // trim the trail ']'
        host = std::string(host.data(), host.size() - 1);
    }

    return true;
}

void AFCWebSocktServer::ProcessMsgLogicThread()
{
    std::list<AFGUID> xNeedRemoveList;
    {
        AFScopeRdLock xGuard(mRWLock);
        for(std::map<AFGUID, BryHttpNetObject*>::iterator iter = mmObject.begin(); iter != mmObject.end(); ++iter)
        {
            ProcessMsgLogicThread(iter->second);
            if(!iter->second->NeedRemove())
            {
                continue;
            }

            xNeedRemoveList.push_back(iter->second->GetClientID());
        }
    }

    for(std::list<AFGUID>::iterator iter = xNeedRemoveList.begin(); iter != xNeedRemoveList.end(); ++iter)
    {
        AFScopeWrLock xGuard(mRWLock);
        RemoveNetObject(*iter);
    }
}

void AFCWebSocktServer::ProcessMsgLogicThread(BryHttpNetObject* pEntity)
{
    //Handle Msg
    size_t nReceiveCount = pEntity->mqMsgFromNet.Count();
    for(size_t i = 0; i < nReceiveCount; ++i)
    {
        MsgFromBryHttpNetInfo* pMsgFromNet(NULL);
        if(!pEntity->mqMsgFromNet.Pop(pMsgFromNet))
        {
            break;
        }

        if(pMsgFromNet == nullptr)
        {
            continue;
        }

        switch(pMsgFromNet->nType)
        {
        case RECIVEDATA:
            {
                int nRet = 0;
                if(mRecvCB)
                {
                    mRecvCB(pMsgFromNet->xHead, pMsgFromNet->xHead.GetMsgID(), pMsgFromNet->strMsg.c_str(), pMsgFromNet->strMsg.size(), pEntity->GetClientID());
                }
            }
            break;
        case CONNECTED:
            {
                mEventCB((NetEventType)pMsgFromNet->nType, pMsgFromNet->xClientID, mnServerID);
            }
            break;
        case DISCONNECTED:
            {
                mEventCB((NetEventType)pMsgFromNet->nType, pMsgFromNet->xClientID, mnServerID);
                pEntity->SetNeedRemove(true);
            }
            break;
        default:
            break;
        }

        delete pMsgFromNet;
    }
}

bool AFCWebSocktServer::Final()
{
    bWorking = false;
    return true;
}

bool AFCWebSocktServer::SendMsgToAllClient(const char* msg, const size_t nLen)
{
    auto frame = std::make_shared<std::string>();
    brynet::net::WebSocketFormat::wsFrameBuild(msg,
            nLen,
            *frame,
            brynet::net::WebSocketFormat::WebSocketFrameType::BINARY_FRAME,
            true,
            false);

    std::map<AFGUID, BryHttpNetObject*>::iterator it = mmObject.begin();
    for(; it != mmObject.end(); ++it)
    {
        BryHttpNetObject* pNetObject = (BryHttpNetObject*)it->second;
        if(pNetObject && !pNetObject->NeedRemove())
        {
            pNetObject->GetConnPtr()->send(frame);
        }
    }

    return true;
}

bool AFCWebSocktServer::SendMsg(const char* msg, const size_t nLen, const AFGUID& xClient)
{
    AFScopeRdLock xGuard(mRWLock);

    BryHttpNetObject* pNetObject = GetNetObject(xClient);
    if(pNetObject == nullptr)
    {
        return false;
    }
    auto frame = std::make_shared<std::string>();
    brynet::net::WebSocketFormat::wsFrameBuild(msg,
            nLen,
            *frame,
            brynet::net::WebSocketFormat::WebSocketFrameType::BINARY_FRAME,
            true,
            false);

    pNetObject->GetConnPtr()->send(frame);
    return true;
}

bool AFCWebSocktServer::AddNetObject(const AFGUID& xClientID, BryHttpNetObject* pEntity)
{
    return mmObject.insert(std::make_pair(xClientID, pEntity)).second;
}

bool AFCWebSocktServer::RemoveNetObject(const AFGUID& xClientID)
{
    BryHttpNetObject* pNetObject = GetNetObject(xClientID);
    if(pNetObject)
    {
        delete pNetObject;
    }
    return mmObject.erase(xClientID);
}

bool AFCWebSocktServer::CloseNetObject(const AFGUID& xClientID)
{
    BryHttpNetObject* pEntity = GetNetObject(xClientID);
    if(pEntity)
    {
        pEntity->GetConnPtr()->postShutdown();
    }

    return true;
}

bool AFCWebSocktServer::DismantleNet(BryHttpNetObject* pEntity)
{
    for(; pEntity->GetBuffLen() >= AFIMsgHead::ARK_MSG_HEAD_LENGTH;)
    {
        AFCMsgHead xHead;
        int nMsgBodyLength = DeCode(pEntity->GetBuff(), pEntity->GetBuffLen(), xHead);
        if(nMsgBodyLength >= 0 && xHead.GetMsgID() > 0)
        {
            MsgFromBryHttpNetInfo* pNetInfo = new  MsgFromBryHttpNetInfo(pEntity->GetConnPtr());
            pNetInfo->xHead = xHead;
            pNetInfo->nType = RECIVEDATA;
            pNetInfo->strMsg.append(pEntity->GetBuff() + AFIMsgHead::ARK_MSG_HEAD_LENGTH, nMsgBodyLength);
            pEntity->mqMsgFromNet.Push(pNetInfo);
            size_t nRet = pEntity->RemoveBuff(nMsgBodyLength + AFIMsgHead::ARK_MSG_HEAD_LENGTH);
        }
        else
        {
            break;
        }
    }

    return true;
}

bool AFCWebSocktServer::CloseSocketAll()
{
    std::map<AFGUID, BryHttpNetObject*>::iterator it = mmObject.begin();
    for(it; it != mmObject.end(); ++it)
    {
        it->second->GetConnPtr()->postShutdown();
        delete it->second;
    }

    mmObject.clear();

    return true;
}

BryHttpNetObject* AFCWebSocktServer::GetNetObject(const AFGUID& xClientID)
{
    std::map<AFGUID, BryHttpNetObject*>::iterator it = mmObject.find(xClientID);
    if(it != mmObject.end())
    {
        return it->second;
    }

    return NULL;
}

bool AFCWebSocktServer::SendMsgWithOutHead(const uint16_t nMsgID, const char* msg, const size_t nLen, const AFGUID& xClientID, const AFGUID& xPlayerID)
{
    std::string strOutData;
    AFCMsgHead xHead;
    xHead.SetMsgID(nMsgID);
    xHead.SetPlayerID(xPlayerID);
    xHead.SetBodyLength(nLen);

    int nAllLen = EnCode(xHead, msg, nLen, strOutData);
    if(nAllLen == nLen + AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return SendMsg(strOutData.c_str(), strOutData.length(), xClientID);
    }

    return false;
}

bool AFCWebSocktServer::SendMsgToAllClientWithOutHead(const uint16_t nMsgID, const char* msg, const size_t nLen, const AFGUID& xPlayerID)
{
    std::string strOutData;
    AFCMsgHead xHead;
    xHead.SetMsgID(nMsgID);
    xHead.SetPlayerID(xPlayerID);

    int nAllLen = EnCode(xHead, msg, nLen, strOutData);
    if(nAllLen == nLen + AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return SendMsgToAllClient(strOutData.c_str(), strOutData.length());
    }

    return false;
}

int AFCWebSocktServer::EnCode(const AFCMsgHead& xHead, const char* strData, const size_t len, std::string& strOutData)
{
    char szHead[AFIMsgHead::ARK_MSG_HEAD_LENGTH] = { 0 };
    int nRet = xHead.EnCode(szHead);

    strOutData.clear();
    strOutData.append(szHead, AFIMsgHead::ARK_MSG_HEAD_LENGTH);
    strOutData.append(strData, len);

    return xHead.GetBodyLength() + AFIMsgHead::ARK_MSG_HEAD_LENGTH;
}

int AFCWebSocktServer::DeCode(const char* strData, const size_t len, AFCMsgHead& xHead)
{
    if(len < AFIMsgHead::ARK_MSG_HEAD_LENGTH)
    {
        return -1;
    }

    if(AFIMsgHead::ARK_MSG_HEAD_LENGTH != xHead.DeCode(strData))
    {
        return -2;
    }

    if(xHead.GetBodyLength() > (len - AFIMsgHead::ARK_MSG_HEAD_LENGTH))
    {
        return -3;
    }

    return xHead.GetBodyLength();
}
