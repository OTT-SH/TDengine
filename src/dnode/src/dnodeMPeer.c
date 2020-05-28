/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "taosmsg.h"
#include "tutil.h"
#include "tqueue.h"
#include "trpc.h"
#include "twal.h"
#include "tglobal.h"
#include "mnode.h"
#include "dnode.h"
#include "dnodeInt.h"
#include "dnodeMgmt.h"
#include "dnodeMWrite.h"

typedef struct {
  pthread_t thread;
  int32_t   workerId;
} SMPeerWorker;

typedef struct {
  int32_t       num;
  SMPeerWorker *peerWorker;
} SMPeerWorkerPool;

static SMPeerWorkerPool tsMPeerPool;
static taos_qset        tsMPeerQset;
static taos_queue       tsMPeerQueue;

static void *dnodeProcessMnodePeerQueue(void *param);

int32_t dnodeInitMnodePeer() {
  tsMPeerQset = taosOpenQset();
  
  tsMPeerPool.num = 1;
  tsMPeerPool.peerWorker = (SMPeerWorker *)calloc(sizeof(SMPeerWorker), tsMPeerPool.num);

  if (tsMPeerPool.peerWorker == NULL) return -1;
  for (int32_t i = 0; i < tsMPeerPool.num; ++i) {
    SMPeerWorker *pWorker = tsMPeerPool.peerWorker + i;
    pWorker->workerId = i;
  }

  dPrint("dnode mpeer is opened");
  return 0;
}

void dnodeCleanupMnodePeer() {
  for (int32_t i = 0; i < tsMPeerPool.num; ++i) {
    SMPeerWorker *pWorker = tsMPeerPool.peerWorker + i;
    if (pWorker->thread) {
      taosQsetThreadResume(tsMPeerQset);
    }
  }

  for (int32_t i = 0; i < tsMPeerPool.num; ++i) {
    SMPeerWorker *pWorker = tsMPeerPool.peerWorker + i;
    if (pWorker->thread) {
      pthread_join(pWorker->thread, NULL);
    }
  }

  dPrint("dnode mmgmt is closed");
}

int32_t dnodeAllocateMnodePqueue() {
  tsMPeerQueue = taosOpenQueue();
  if (tsMPeerQueue == NULL) return TSDB_CODE_SERV_OUT_OF_MEMORY;

  taosAddIntoQset(tsMPeerQset, tsMPeerQueue, NULL);

  for (int32_t i = 0; i < tsMPeerPool.num; ++i) {
    SMPeerWorker *pWorker = tsMPeerPool.peerWorker + i;
    pWorker->workerId = i;

    pthread_attr_t thAttr;
    pthread_attr_init(&thAttr);
    pthread_attr_setdetachstate(&thAttr, PTHREAD_CREATE_JOINABLE);

    if (pthread_create(&pWorker->thread, &thAttr, dnodeProcessMnodePeerQueue, pWorker) != 0) {
      dError("failed to create thread to process mmgmt queue, reason:%s", strerror(errno));
    }

    pthread_attr_destroy(&thAttr);
    dTrace("dnode mmgmt worker:%d is launched, total:%d", pWorker->workerId, tsMPeerPool.num);
  }

  dTrace("dnode mmgmt queue:%p is allocated", tsMPeerQueue);
  return TSDB_CODE_SUCCESS;
}

void dnodeFreeMnodePqueue() {
  taosCloseQueue(tsMPeerQueue);
  tsMPeerQueue = NULL;
}

void dnodeDispatchToMnodePeerQueue(SRpcMsg *pMsg) {
  if (!mnodeIsRunning() || tsMPeerQueue == NULL) {
    dnodeSendRedirectMsg(pMsg->msgType, pMsg->handle, false);
    return;
  }

  SMnodeMsg *pPeer = (SMnodeMsg *)taosAllocateQitem(sizeof(SMnodeMsg));
  mnodeCreateMsg(pPeer, pMsg);
  taosWriteQitem(tsMPeerQueue, TAOS_QTYPE_RPC, pPeer);
}

static void dnodeSendRpcMnodePeerRsp(SMnodeMsg *pPeer, int32_t code) {
  if (code == TSDB_CODE_ACTION_IN_PROGRESS) return;

  SRpcMsg rpcRsp = {
    .handle  = pPeer->thandle,
    .pCont   = pPeer->rpcRsp.rsp,
    .contLen = pPeer->rpcRsp.len,
    .code    = code,
  };

  rpcSendResponse(&rpcRsp);
  mnodeCleanupMsg(pPeer);
}

static void *dnodeProcessMnodePeerQueue(void *param) {
  SMnodeMsg *pPeerMsg;
  int32_t    type;
  void *     unUsed;
  
  while (1) {
    if (taosReadQitemFromQset(tsMPeerQset, &type, (void **)&pPeerMsg, &unUsed) == 0) {
      dTrace("dnodeProcessMnodePeerQueue: got no message from qset, exiting...");
      break;
    }

    dTrace("%p, msg:%s will be processed in mpeer queue", pPeerMsg->ahandle, taosMsg[pPeerMsg->msgType]);    
    int32_t code = mnodeProcessPeerReq(pPeerMsg);    
    dnodeSendRpcMnodePeerRsp(pPeerMsg, code);    
    taosFreeQitem(pPeerMsg);
  }

  return NULL;
}