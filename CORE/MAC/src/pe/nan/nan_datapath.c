/*
 * Copyright (c) 2016 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * DOC: nan_datapath.c
 *
 * MAC NAN Data path API implementation
 */

#include "limUtils.h"
#include "limApi.h"
#include "limAssocUtils.h"
#include "nan_datapath.h"
#include "limTypes.h"
#include "limSendMessages.h"
#include "wma_nan_datapath.h"

/**
 * lim_send_ndp_event_to_sme() - generic function to prepare and send NDP
 * message to SME.
 * @mac_ctx: handle to mac context structure
 * @msg_type: sme message type to send
 * @body_ptr: buffer
 * @len: buffer length
 * @body_val: value
 *
 * Return: None
 */
static void lim_send_ndp_event_to_sme(tpAniSirGlobal mac_ctx, uint32_t msg_type,
				void *body_ptr, uint32_t len, uint32_t body_val)
{
	tSirMsgQ mmh_msg = {0};

	mmh_msg.type = msg_type;
	if (len && body_ptr) {
		mmh_msg.bodyptr = vos_mem_malloc(len);
		if (NULL == mmh_msg.bodyptr) {
			limLog(mac_ctx, LOGE, FL("Malloc failed"));
			return;
		}
		vos_mem_copy(mmh_msg.bodyptr, body_ptr, len);
	} else {
		mmh_msg.bodyval = body_val;
	}
	limSysProcessMmhMsgApi(mac_ctx, &mmh_msg, ePROT);
}

/**
 * lim_add_ndi_peer() - Function to add ndi peer
 * @mac_ctx: handle to mac structure
 * @vdev_id: vdev id on which peer is added
 * @peer_mac_addr: peer to be added
 *
 * Return: VOS_STATUS_SUCCESS on success; error number otherwise
 */
static VOS_STATUS lim_add_ndi_peer(tpAniSirGlobal mac_ctx,
	uint32_t vdev_id, v_MACADDR_t peer_mac_addr)
{
	tpPESession session;
	tpDphHashNode sta_ds;
	uint16_t assoc_id, peer_idx;
	tSirRetStatus status;

	session = pe_find_session_by_sme_session_id(mac_ctx,
						vdev_id);
	if (session == NULL) {
		/* couldn't find session */
		return VOS_STATUS_E_FAILURE;
	}
	sta_ds = dphLookupHashEntry(mac_ctx,
				peer_mac_addr.bytes,
				&assoc_id, &session->dph.dphHashTable);
	/* peer exists, don't do anything */
	if (sta_ds != NULL) {
		limLog(mac_ctx, LOGE, FL("NDI Peer already exists!!"));
		return VOS_STATUS_SUCCESS;
	}
	limLog(mac_ctx, LOG1,
		FL("Need to create NDI Peer :" MAC_ADDRESS_STR),
		MAC_ADDR_ARRAY(peer_mac_addr.bytes));
	peer_idx = limAssignPeerIdx(mac_ctx, session);
	sta_ds = dphAddHashEntry(mac_ctx, peer_mac_addr.bytes, peer_idx,
			&session->dph.dphHashTable);
	if (sta_ds == NULL) {
		limLog(mac_ctx, LOGE,
			FL("Couldn't add dph entry"));
		/* couldn't add dph entry */
		return VOS_STATUS_E_FAILURE;
	}
	/* wma decides NDI mode from wma->inferface struct */
	sta_ds->staType = STA_ENTRY_NDI_PEER;
	status = limAddSta(mac_ctx, sta_ds, false, session);
	if (eSIR_SUCCESS != status) {
		/* couldn't add peer */
		limLog(mac_ctx, LOGE,
			FL("limAddSta failed status: %d"),
			status);
		return VOS_STATUS_E_FAILURE;
	}
	return VOS_STATUS_SUCCESS;
}

/**
 * lim_handle_ndp_indication_event() - Function to handle SIR_HAL_NDP_INDICATION
 * event from WMA
 * @mac_ctx: handle to mac structure
 * @ndp_ind: ndp indication event params
 *
 * Return: VOS_STATUS_SUCCESS on success; error number otherwise
 */
static VOS_STATUS lim_handle_ndp_indication_event(tpAniSirGlobal mac_ctx,
					struct ndp_indication_event *ndp_ind)
{
	VOS_STATUS status = VOS_STATUS_SUCCESS;

	limLog(mac_ctx, LOG1,
		FL("role: %d, vdev: %d, peer_mac_addr "MAC_ADDRESS_STR),
		ndp_ind->role, ndp_ind->vdev_id,
		MAC_ADDR_ARRAY(ndp_ind->peer_mac_addr.bytes));

	if ((ndp_ind->role == NDP_ROLE_INITIATOR) ||
	   ((NDP_ROLE_RESPONDER == ndp_ind->role) &&
	   (NDP_ACCEPT_POLICY_ALL == ndp_ind->policy))) {
		status = lim_add_ndi_peer(mac_ctx, ndp_ind->vdev_id,
				ndp_ind->peer_mac_addr);
		if (VOS_STATUS_SUCCESS != status) {
			limLog(mac_ctx, LOGE,
				FL("Couldn't add ndi peer, ndp_role: %d"),
				ndp_ind->role);
			goto ndp_indication_failed;
		}
	}

	if (NDP_ROLE_RESPONDER == ndp_ind->role)
		lim_send_ndp_event_to_sme(mac_ctx, eWNI_SME_NDP_INDICATION,
			ndp_ind, sizeof(*ndp_ind), 0);

	/*
	 * With NDP indication if peer does not exists already add_sta is
	 * executed resulting in new peer else no action is taken. Note that
	 * new_peer event is not necessary event and should not be sent if case
	 * anything fails in this function. Rather eWNI_SME_NDP_CONFIRM_IND is
	 * used to indicate success of final operation and abscence of it can be
	 * used by service layer to identify failure.
	 */

ndp_indication_failed:
	/*
	 * Free config if failure or for NDP_ROLE_INITIATOR role
	 * As for success responder case this info is sent till HDD
	 * and will be freed in sme.
	 */
	if ((status != VOS_STATUS_SUCCESS) ||
			(NDP_ROLE_INITIATOR == ndp_ind->role)) {
		vos_mem_free(ndp_ind->ndp_config.ndp_cfg);
		vos_mem_free(ndp_ind->ndp_info.ndp_app_info);
	}
	return status;
}

/**
 * lim_ndp_responder_rsp_handler() - Handler for NDP responder rsp
 * @mac_ctx: handle to mac structure
 * @ndp_rsp: pointer to rsp message
 * @bodyval: value
 *
 * Return: VOS_STATUS_SUCCESS on success; error number otherwise
 */
static VOS_STATUS lim_ndp_responder_rsp_handler(tpAniSirGlobal mac_ctx,
	struct ndp_responder_rsp_event *rsp_ind, uint32_t bodyval)
{
	VOS_STATUS ret_val = VOS_STATUS_SUCCESS;

	if ((NULL == rsp_ind) || bodyval) {
		limLog(mac_ctx, LOGE,
			FL("rsp_ind is NULL or bodyval %d"), bodyval);
		/* msg to unblock SME, but not send rsp to HDD */
		bodyval = true;
		ret_val = VOS_STATUS_E_INVAL;
		goto responder_rsp;
	}

	if (VOS_STATUS_SUCCESS == rsp_ind->status) {
		ret_val = lim_add_ndi_peer(mac_ctx, rsp_ind->vdev_id,
				rsp_ind->peer_mac_addr);
		if (VOS_STATUS_SUCCESS != ret_val) {
			limLog(mac_ctx, LOGE,
				FL("Couldn't add ndi peer"));
			rsp_ind->status = VOS_STATUS_E_FAILURE;
		}
	}

responder_rsp:
	/* send eWNI_SME_NDP_RESPONDER_RSP */
	lim_send_ndp_event_to_sme(mac_ctx, eWNI_SME_NDP_RESPONDER_RSP,
				bodyval ? NULL : rsp_ind,
				bodyval ? 0 : sizeof(*rsp_ind), bodyval);
	return ret_val;
}

/**
 * lim_handle_ndp_event_message() - Handler for NDP events/RSP from WMA
 * @mac_ctx: handle to mac structure
 * @msg: pointer to message
 *
 * Return: VOS_STATUS_SUCCESS on success; error number otherwise
 */
VOS_STATUS lim_handle_ndp_event_message(tpAniSirGlobal mac_ctx, tpSirMsgQ msg)
{
	VOS_STATUS status = VOS_STATUS_SUCCESS;

	switch (msg->type) {
	case SIR_HAL_NDP_CONFIRM:
		lim_send_ndp_event_to_sme(mac_ctx, eWNI_SME_NDP_CONFIRM_IND,
				msg->bodyptr, sizeof(struct ndp_confirm_event),
				msg->bodyval);
		break;
	case SIR_HAL_NDP_INITIATOR_RSP:
		lim_send_ndp_event_to_sme(mac_ctx, eWNI_SME_NDP_INITIATOR_RSP,
				msg->bodyptr, sizeof(struct ndp_initiator_rsp),
				msg->bodyval);
		break;
	case SIR_HAL_NDP_INDICATION: {
		struct ndp_indication_event *ndp_ind = msg->bodyptr;
		status = lim_handle_ndp_indication_event(mac_ctx, ndp_ind);
		break;
	}
	case SIR_HAL_NDP_RESPONDER_RSP:
		status = lim_ndp_responder_rsp_handler(mac_ctx, msg->bodyptr,
					msg->bodyval);
		break;
	default:
		limLog(mac_ctx, LOGE, FL("Unhandled NDP event: %d"), msg->type);
		status = VOS_STATUS_E_NOSUPPORT;
		break;
	}
	vos_mem_free(msg->bodyptr);
	return status;
}

/**
 * lim_process_sme_ndp_initiator_req() - Handler for eWNI_SME_NDP_INITIATOR_REQ
 * from SME.
 * @mac_ctx: handle to mac structure
 * @ndp_msg: ndp initiator request msg
 *
 * Return: Status of operation
 */
VOS_STATUS lim_process_sme_ndp_initiator_req(tpAniSirGlobal mac_ctx,
					     void *ndp_msg)
{
	tSirMsgQ msg;
	VOS_STATUS status;

	struct sir_sme_ndp_initiator_req *sme_req =
		(struct sir_sme_ndp_initiator_req *)ndp_msg;
	struct ndp_initiator_req *wma_req;

	if (NULL == ndp_msg) {
		limLog(mac_ctx, LOGE, FL("invalid ndp_req"));
		status = VOS_STATUS_E_INVAL;
		goto send_initiator_rsp;
	}
	wma_req = vos_mem_malloc(sizeof(*wma_req));
	if (wma_req == NULL) {
		limLog(mac_ctx, LOGE, FL("malloc failed"));
		status = VOS_STATUS_E_NOMEM;
		goto send_initiator_rsp;
	}

	vos_mem_copy(wma_req, &sme_req->req, sizeof(*wma_req));
	msg.type = SIR_HAL_NDP_INITIATOR_REQ;
	msg.reserved = 0;
	msg.bodyptr = wma_req;
	msg.bodyval = 0;

	limLog(mac_ctx, LOG1, FL("sending WDA_NDP_INITIATOR_REQ to WMA"));
	MTRACE(macTraceMsgTx(mac_ctx, NO_SESSION, msg.type));

	if (eSIR_SUCCESS != wdaPostCtrlMsg(mac_ctx, &msg))
		limLog(mac_ctx, LOGP, FL("wdaPostCtrlMsg failed"));

	return VOS_STATUS_SUCCESS;
send_initiator_rsp:
	/* msg to unblock SME, but not send rsp to HDD */
	lim_send_ndp_event_to_sme(mac_ctx, eWNI_SME_NDP_INITIATOR_RSP,
				  NULL, 0, true);
	return status;
}

/**
 * lim_process_sme_ndp_responder_req() - Handler for NDP responder req
 * @mac_ctx: handle to mac structure
 * @ndp_msg: pointer to message
 *
 * Return: VOS_STATUS_SUCCESS on success or failure code in case of failure
 */
static VOS_STATUS lim_process_sme_ndp_responder_req(tpAniSirGlobal mac_ctx,
	struct sir_sme_ndp_responder_req *lim_msg)
{
	tSirMsgQ msg;
	VOS_STATUS status = VOS_STATUS_SUCCESS;
	struct ndp_responder_req *responder_req;

	if (NULL == lim_msg) {
		limLog(mac_ctx, LOGE, FL("ndp_msg is NULL"));
		status = VOS_STATUS_E_INVAL;
		goto send_failure_rsp;
	}
	responder_req = vos_mem_malloc(sizeof(*responder_req));
	if (NULL == responder_req) {
		limLog(mac_ctx, LOGE,
			FL("Unable to allocate memory for responder_req"));
		status = VOS_STATUS_E_NOMEM;
		goto send_failure_rsp;
	}
	vos_mem_copy(responder_req, &lim_msg->req, sizeof(*responder_req));
	msg.type = SIR_HAL_NDP_RESPONDER_REQ;
	msg.reserved = 0;
	msg.bodyptr = responder_req;
	msg.bodyval = 0;

	limLog(mac_ctx, LOG1, FL("sending SIR_HAL_NDP_RESPONDER_REQ to WMA"));
	MTRACE(macTraceMsgTx(mac_ctx, NO_SESSION, msg.type));

	if (eSIR_SUCCESS != wdaPostCtrlMsg(mac_ctx, &msg)) {
		limLog(mac_ctx, LOGE, FL("wdaPostCtrlMsg failed"));
		status = VOS_STATUS_E_FAILURE;
		vos_mem_free(responder_req);
		goto send_failure_rsp;
	}
	return status;
send_failure_rsp:
	/* msg to unblock SME, but not send rsp to HDD */
	lim_send_ndp_event_to_sme(mac_ctx, eWNI_SME_NDP_RESPONDER_RSP,
				NULL, 0, true);
	return status;
}

/**
 * lim_handle_ndp_request_message() - Handler for NDP req from SME
 * @mac_ctx: handle to mac structure
 * @msg: pointer to message
 *
 * Return: VOS_STATUS_SUCCESS on success; error number otherwise
 */
VOS_STATUS lim_handle_ndp_request_message(tpAniSirGlobal mac_ctx,
					  tpSirMsgQ msg)
{
	VOS_STATUS status;

	switch (msg->type) {
	case eWNI_SME_NDP_INITIATOR_REQ:
		status = lim_process_sme_ndp_initiator_req(mac_ctx,
							   msg->bodyptr);
		break;
	case eWNI_SME_NDP_RESPONDER_REQ:
		status = lim_process_sme_ndp_responder_req(mac_ctx,
							 msg->bodyptr);
		break;
	default:
		limLog(mac_ctx, LOGE, FL("Unhandled NDP request: %d"),
		       msg->type);
		status = VOS_STATUS_E_NOSUPPORT;
		break;
	}
	return status;
}

/**
 * lim_process_ndi_mlm_add_bss_rsp() - Process ADD_BSS response for NDI
 * @mac_ctx: Pointer to Global MAC structure
 * @lim_msgq: The MsgQ header, which contains the response buffer
 * @session_entry: PE session
 *
 * Return: None
 */
void lim_process_ndi_mlm_add_bss_rsp(tpAniSirGlobal mac_ctx, tpSirMsgQ lim_msgq,
					tpPESession session_entry)
{
	tLimMlmStartCnf mlm_start_cnf;
	tpAddBssParams add_bss_params = (tpAddBssParams) lim_msgq->bodyptr;

	limLog(mac_ctx, LOG1, FL("Status %d"), add_bss_params->status);
	if (NULL == add_bss_params) {
		limLog(mac_ctx, LOGE, FL("Invalid body pointer in message"));
		goto end;
	}
	if (eHAL_STATUS_SUCCESS == add_bss_params->status) {
		limLog(mac_ctx, LOG1,
		       FL("WDA_ADD_BSS_RSP returned eHAL_STATUS_SUCCESS"));
		session_entry->limMlmState = eLIM_MLM_BSS_STARTED_STATE;
		MTRACE(macTrace(mac_ctx, TRACE_CODE_MLM_STATE,
			session_entry->peSessionId,
			session_entry->limMlmState));
		session_entry->bssIdx = (uint8_t) add_bss_params->bssIdx;
		session_entry->limSystemRole = eLIM_NDI_ROLE;
		session_entry->statypeForBss = STA_ENTRY_SELF;
		/* Apply previously set configuration at HW */
		limApplyConfiguration(mac_ctx, session_entry);
		mlm_start_cnf.resultCode = eSIR_SME_SUCCESS;
	} else {
		limLog(mac_ctx, LOGE,
			FL("WDA_ADD_BSS_REQ failed with status %d"),
			add_bss_params->status);
		mlm_start_cnf.resultCode = eSIR_SME_HAL_SEND_MESSAGE_FAIL;
	}
	mlm_start_cnf.sessionId = session_entry->peSessionId;
	limPostSmeMessage(mac_ctx, LIM_MLM_START_CNF,
				(uint32_t *) &mlm_start_cnf);
end:
	vos_mem_free(lim_msgq->bodyptr);
	lim_msgq->bodyptr = NULL;
}

/**
 * lim_ndi_del_bss_rsp() - Handler DEL BSS resp for NDI interface
 * @mac_ctx: handle to mac structure
 * @msg: pointer to message
 * @session_entry: session entry
 *
 * Return: void
 */
void lim_ndi_del_bss_rsp(tpAniSirGlobal  mac_ctx,
			void *msg, tpPESession session_entry)
{
	tSirResultCodes rc = eSIR_SME_SUCCESS;
	tpDeleteBssParams del_bss = (tpDeleteBssParams) msg;

	SET_LIM_PROCESS_DEFD_MESGS(mac_ctx, true);
	if (del_bss == NULL) {
		limLog(mac_ctx, LOGE,
			FL("NDI: DEL_BSS_RSP with no body!"));
		rc = eSIR_SME_STOP_BSS_FAILURE;
		goto end;
	}
	session_entry =
		peFindSessionBySessionId(mac_ctx, del_bss->sessionId);
	if (!session_entry) {
		limLog(mac_ctx, LOGE,
			FL("Session Does not exist for given sessionID"));
		goto end;
	}

	if (del_bss->status != eHAL_STATUS_SUCCESS) {
		limLog(mac_ctx, LOGE, FL("NDI: DEL_BSS_RSP error (%x) Bss %d "),
			del_bss->status, del_bss->bssIdx);
		rc = eSIR_SME_STOP_BSS_FAILURE;
		goto end;
	}

	if (limSetLinkState(mac_ctx, eSIR_LINK_IDLE_STATE,
			session_entry->selfMacAddr,
			session_entry->selfMacAddr, NULL, NULL)
			!= eSIR_SUCCESS) {
		limLog(mac_ctx, LOGE,
			FL("NDI: DEL_BSS_RSP setLinkState failed"));
		goto end;
	}

	session_entry->limMlmState = eLIM_MLM_IDLE_STATE;

end:
	if (del_bss)
		vos_mem_free(del_bss);
	/* Delete PE session once BSS is deleted */
	if (NULL != session_entry) {
		limSendSmeRsp(mac_ctx, eWNI_SME_STOP_BSS_RSP,
			rc, session_entry->smeSessionId,
			session_entry->transactionId);
		peDeleteSession(mac_ctx, session_entry);
		session_entry = NULL;
	}
}

/**
 * lim_send_sme_ndp_add_sta_rsp() - prepares and send new peer ind to SME
 * @mac_ctx: handle to mac structure
 * @session: session pointer
 * @add_sta_rsp: add sta response struct
 *
 * Return: status of operation
 */
static VOS_STATUS lim_send_sme_ndp_add_sta_rsp(tpAniSirGlobal mac_ctx,
					       tpPESession session,
					       tAddStaParams *add_sta_rsp)
{
	tSirMsgQ  mmh_msg = {0};
	struct sme_ndp_peer_ind *new_peer_ind;

	mmh_msg.type = eWNI_SME_NDP_NEW_PEER_IND;

	if (NULL == add_sta_rsp) {
		limLog(mac_ctx, LOGE, FL("Invalid add_sta_rsp"));
		return VOS_STATUS_E_INVAL;
	}

	new_peer_ind = vos_mem_malloc(sizeof(*new_peer_ind));
	if (NULL == new_peer_ind) {
		limLog(mac_ctx, LOGE, FL("Failed to allocate memory"));
		return VOS_STATUS_E_NOMEM;
	}

	/* this message is going to HDD, fill in sme session id */
	new_peer_ind->session_id = add_sta_rsp->smesessionId;
	new_peer_ind->msg_len = sizeof(struct sme_ndp_peer_ind);
	new_peer_ind->msg_type = eWNI_SME_NDP_NEW_PEER_IND;
	vos_mem_copy(new_peer_ind->peer_mac_addr.bytes, add_sta_rsp->staMac,
		     sizeof(tSirMacAddr));
	new_peer_ind->sta_id = add_sta_rsp->staIdx;

	mmh_msg.bodyptr = new_peer_ind;
	mmh_msg.bodyval = 0;
	limSysProcessMmhMsgApi(mac_ctx, &mmh_msg, ePROT);
	return VOS_STATUS_SUCCESS;
}

/**
 * lim_ndp_add_sta_rsp() - handles add sta rsp for NDP from WMA
 * @mac_ctx: handle to mac structure
 * @session: session pointer
 * @add_sta_rsp: add sta response struct
 *
 * Return: None
 */
void lim_ndp_add_sta_rsp(tpAniSirGlobal mac_ctx, tpPESession session,
			 tAddStaParams *add_sta_rsp)
{
	tpDphHashNode sta_ds;
	uint16_t peer_idx;

	if (NULL == add_sta_rsp) {
		limLog(mac_ctx, LOGE, FL("Invalid add_sta_rsp"));
		vos_mem_free(add_sta_rsp);
		return;
	}

	SET_LIM_PROCESS_DEFD_MESGS(mac_ctx, true);
	sta_ds = dphLookupHashEntry(mac_ctx, add_sta_rsp->staMac, &peer_idx,
				    &session->dph.dphHashTable);
	if (sta_ds == NULL) {
		limLog(mac_ctx, LOGE,
			FL("NAN: ADD_STA_RSP for unknown MAC addr "
			MAC_ADDRESS_STR),
			MAC_ADDR_ARRAY(add_sta_rsp->staMac));
		vos_mem_free(add_sta_rsp);
		return;
	}

	if (add_sta_rsp->status != eHAL_STATUS_SUCCESS) {
		limLog(mac_ctx, LOGE,
			FL("NAN: ADD_STA_RSP error %x for MAC addr: %pM"),
			add_sta_rsp->status, add_sta_rsp->staMac);
		/* delete the sta_ds allocated during ADD STA */
		limDeleteDphHashEntry(mac_ctx, add_sta_rsp->staMac,
				      peer_idx, session);
		vos_mem_free(add_sta_rsp);
		return;
	}
	sta_ds->bssId = add_sta_rsp->bssIdx;
	sta_ds->staIndex = add_sta_rsp->staIdx;
	sta_ds->ucUcastSig = add_sta_rsp->ucUcastSig;
	sta_ds->ucBcastSig = add_sta_rsp->ucBcastSig;
	sta_ds->valid = 1;
	sta_ds->mlmStaContext.mlmState = eLIM_MLM_LINK_ESTABLISHED_STATE;
	lim_send_sme_ndp_add_sta_rsp(mac_ctx, session, add_sta_rsp);
	vos_mem_free(add_sta_rsp);
}
