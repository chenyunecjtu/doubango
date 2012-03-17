/*
* Copyright (C) 2010-2011 Mamadou Diop.
*
* Contact: Mamadou Diop <diopmamadou(at)doubango.org>
*	
* This file is part of Open Source Doubango Framework.
*
* DOUBANGO is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*	
* DOUBANGO is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*	
* You should have received a copy of the GNU General Public License
* along with DOUBANGO.
*
*/
/**@file trtp_manager.c
 * @brief RTP/RTCP manager.
 *
 * @author Mamadou Diop <diopmamadou(at)doubango.org>
 *

 */
#include "tinyrtp/trtp_manager.h"

#include "tinyrtp/rtp/trtp_rtp_packet.h"

#include "tsk_string.h"
#include "tsk_memory.h"
#include "tsk_base64.h"
#include "tsk_debug.h"

#define TINY_RCVBUF					(256/2/*Will be doubled and min on linux is 256*/) /* tiny buffer used to disable receiving */
#define BIG_RCVBUF					(64 * 1024)
#define BIG_SNDBUF					(64 * 1024)

#if !defined(TRTP_PORT_RANGE_START)
#	define TRTP_PORT_RANGE_START 1024
#endif
#if !defined(TRTP_PORT_RANGE_STOP)
#	define TRTP_PORT_RANGE_STOP 65535
#endif

/* ======================= Transport callback ========================== */
static int _trtp_transport_layer_cb(const tnet_transport_event_t* e)
{
	int ret = -1;
	const trtp_manager_t *manager = e->callback_data;
	trtp_rtp_packet_t* packet = tsk_null;
	void* data_ptr;
	int data_size;

	switch(e->type){
		case event_data: {
				break;
			}
		case event_closed:
		case event_connected:
		default:{
				return 0;
			}
	}

	//
	//	RTCP
	//
	if(manager->rtcp.local_socket && manager->rtcp.local_socket->fd == e->local_fd){
		data_ptr = (void*)(e->data);
		data_size = (int)(e->size);
#if HAVE_SRTP
				if(manager->srtp_ctx_neg_remote){
					if(srtp_unprotect_rtcp(manager->srtp_ctx_neg_remote->session, data_ptr, &data_size) != err_status_ok){
						TSK_DEBUG_ERROR("srtp_unprotect() failed");
						goto bail;
					}
				}
#endif

		TSK_DEBUG_INFO("RTCP packet");
	}
	//
	// RTP
	//
	else if(manager->transport->master && (manager->transport->master->fd == e->local_fd)){
		data_ptr = (void*)(e->data);
		data_size = (int)(e->size);
		if(manager->rtp.callback){
#if HAVE_SRTP
				if(manager->srtp_ctx_neg_remote){
					if(srtp_unprotect(manager->srtp_ctx_neg_remote->session, data_ptr, &data_size) != err_status_ok){
						TSK_DEBUG_ERROR("srtp_unprotect() failed");
						goto bail;
					}
				}
#endif
			if((packet = trtp_rtp_packet_deserialize(data_ptr, data_size))){
				manager->rtp.callback(manager->rtp.callback_data, packet);
				TSK_OBJECT_SAFE_FREE(packet);
			}
			else{
				TSK_DEBUG_ERROR("RTP packet === NOK");
				goto bail;
			}
		}
	}
	//
	// UNKNOWN
	//
	else{
		TSK_DEBUG_INFO("XXXX packet");
		goto bail;
	}

bail:

	return ret;
}


static int _trtp_manager_enable_sockets(trtp_manager_t* self)
{
	int rcv_buf = BIG_RCVBUF;
	int snd_buf = BIG_SNDBUF;
	int ret;

	if(!self->socket_disabled){
		return 0;
	}

	if(!self || !self->transport){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}

	if((ret = setsockopt(self->transport->master->fd, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_buf, sizeof(rcv_buf)))){
		TNET_PRINT_LAST_ERROR("setsockopt(SOL_SOCKET, SO_RCVBUF) has failed with error code %d", ret);
		return ret;
	}
	if((ret = setsockopt(self->transport->master->fd, SOL_SOCKET, SO_SNDBUF, (char*)&snd_buf, sizeof(snd_buf)))){
		TNET_PRINT_LAST_ERROR("setsockopt(SOL_SOCKET, SO_RCVBUF) has failed with error code %d", ret);
		return ret;
	}

	self->socket_disabled = tsk_false;
	return 0;
}

/** Create RTP/RTCP manager */
trtp_manager_t* trtp_manager_create(tsk_bool_t enable_rtcp, const char* local_ip, tsk_bool_t ipv6)
{
	trtp_manager_t* manager;

#if HAVE_SRTP
	static tsk_bool_t __strp_initialized = tsk_false;
	err_status_t srtp_err;
	if(!__strp_initialized){
		if((srtp_err = srtp_init()) != err_status_ok){
			TSK_DEBUG_ERROR("srtp_init() failed with error code = %d", srtp_err);
		}
		__strp_initialized = (srtp_err == err_status_ok);
	}
#endif

	if((manager = tsk_object_new(trtp_manager_def_t))){
		manager->enable_rtcp = enable_rtcp;
		manager->local_ip = tsk_strdup(local_ip);
		manager->ipv6 = ipv6;
		manager->rtp.payload_type = 127;
	}
	return manager;
}

/** Prepares the RTP/RTCP manager */
int trtp_manager_prepare(trtp_manager_t* self)
{
	uint8_t retry_count = 7;
	tnet_socket_type_t socket_type;

	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	if(self->transport){
		TSK_DEBUG_ERROR("RTP/RTCP manager already prepared");
		return -2;
	}

	socket_type = self->ipv6 ? tnet_socket_type_udp_ipv6 : tnet_socket_type_udp_ipv4;
	
	/* Creates local rtp and rtcp sockets */
	while(retry_count--){
		/* random number in the range 1024 to 65535 */
#if 0
		tnet_port_t local_port = 6060;
#else
		tnet_port_t local_port = ((rand() % (self->port_range.stop - self->port_range.start)) + self->port_range.start);
#endif
		local_port = (local_port & 0xFFFE); /* turn to even number */
		
		/* beacuse failure will cause errors in the log, print a message to alert that there is
		* nothing to worry about */
		TSK_DEBUG_INFO("RTP/RTCP manager[Begin]: Trying to bind to random ports");
		
		/* RTP */
		if((self->transport = tnet_transport_create(self->local_ip, local_port, socket_type, "RTP/RTCP Manager"))){
			/* set callback function */
			tnet_transport_set_callback(self->transport, _trtp_transport_layer_cb, self);
			tsk_strupdate(&self->rtp.public_ip, self->transport->master->ip);
			self->rtp.public_port = local_port;
			/* Disable receiving until we start the transport (To avoid buffering) */
			if(!self->socket_disabled){
				int err, optval = TINY_RCVBUF;
				if((err = setsockopt(self->transport->master->fd, SOL_SOCKET, SO_RCVBUF, (char*)&optval, sizeof(optval)))){
					TNET_PRINT_LAST_ERROR("setsockopt(SOL_SOCKET, SO_RCVBUF) has failed with error code %d", err);
				}
				self->socket_disabled = (err == 0);
			}
		}
		else {
			TSK_DEBUG_ERROR("Failed to create RTP/RTCP Transport");
			return -3;
		}

		/* RTCP */
		if(self->enable_rtcp){
			if(!(self->rtcp.local_socket = tnet_socket_create(self->local_ip, local_port+1, socket_type))){
				TSK_DEBUG_WARN("Failed to bind to %d", local_port+1);
				TSK_OBJECT_SAFE_FREE(self->transport);
				continue;
			}
			else{
				tsk_strupdate(&self->rtcp.public_ip, self->rtcp.local_socket->ip);
				self->rtcp.public_port = local_port + 1;
			}
		}
	
		TSK_DEBUG_INFO("RTP/RTCP manager[End]: Trying to bind to random ports");
		break;
	}

	/* SRTP */
#if HAVE_SRTP
	{		
		trtp_srtp_ctx_init(&self->srtp_contexts[TRTP_SRTP_LINE_IDX_LOCAL][0], 1, HMAC_SHA1_80, self->rtp.ssrc);
		trtp_srtp_ctx_init(&self->srtp_contexts[TRTP_SRTP_LINE_IDX_LOCAL][1], 2, HMAC_SHA1_32, self->rtp.ssrc);
	}
#endif

	return 0;
}

/** Indicates whether the manager is already prepared or not */
tsk_bool_t trtp_manager_is_prepared(trtp_manager_t* self)
{
	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return tsk_false;
	}
	return self->transport == tsk_null ? tsk_false : tsk_true;
}

/** Sets NAT Traversal context */
int trtp_manager_set_natt_ctx(trtp_manager_t* self, tnet_nat_context_handle_t* natt_ctx)
{
	int ret;

	if(!self || !self->transport || !natt_ctx){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	if(!(ret = tnet_transport_set_natt_ctx(self->transport, natt_ctx))){
		tnet_ip_t public_ip = {0};
		tnet_port_t public_port = 0;
		// get RTP public IP and Port
		if(!tnet_transport_get_public_ip_n_port(self->transport, self->transport->master->fd, &public_ip, &public_port)){
			tsk_strupdate(&self->rtp.public_ip, public_ip);
			self->rtp.public_port = public_port;
		}
		// get RTCP public IP and Port
		memset(public_ip, 0, sizeof(public_ip));
		public_port = 0;
		if(self->rtcp.local_socket && !tnet_transport_get_public_ip_n_port(self->transport, self->rtcp.local_socket->fd, &public_ip, &public_port)){
			tsk_strupdate(&self->rtcp.public_ip, public_ip);
			self->rtcp.public_port = public_port;
		}
		// re-enable sockets to be able to receive STUN packets
		_trtp_manager_enable_sockets(self);
	}
	return ret;
}

/** Sets RTP callback */
int trtp_manager_set_rtp_callback(trtp_manager_t* self, trtp_manager_rtp_cb_f callback, const void* callback_data)
{
	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	
	self->rtp.callback = callback;
	self->rtp.callback_data = callback_data;

	return 0;
}

/** Sets the payload type */
int trtp_manager_set_payload_type(trtp_manager_t* self, uint8_t payload_type)
{
	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	self->rtp.payload_type = payload_type;
	return 0;
}

/** Sets remote parameters for rtp session */
int trtp_manager_set_rtp_remote(trtp_manager_t* self, const char* remote_ip, tnet_port_t remote_port)
{
	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	tsk_strupdate(&self->rtp.remote_ip, remote_ip);
	self->rtp.remote_port = remote_port;
	return 0;
}

/** Sets remote parameters for rtcp session */
int trtp_manager_set_rtcp_remote(trtp_manager_t* self, const char* remote_ip, tnet_port_t remote_port)
{
	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	tsk_strupdate(&self->rtcp.remote_ip, remote_ip);
	self->rtcp.remote_port = remote_port;
	return 0;
}

int trtp_manager_set_port_range(trtp_manager_t* self, uint16_t start, uint16_t stop)
{
	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}
	self->port_range.start = start;
	self->port_range.stop = stop;
	return 0;
}

/** Starts the RTP/RTCP manager */
int trtp_manager_start(trtp_manager_t* self)
{
	int ret = 0;

	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}

	if(self->started){
		TSK_DEBUG_WARN("RTP/RTCP manager already started");
		return 0;
	}

	if(!self->transport){
		TSK_DEBUG_ERROR("RTP/RTCP manager not prepared");
		return -2;
	}

	/* Flush buffers and re-enable sockets */
	if(self->transport->master){
		char buff[2048];
		
		// re-enable sockets
		_trtp_manager_enable_sockets(self);
		
		TSK_DEBUG_INFO("Start flushing RTP socket...");
		// Buffer should be empty ...but who know?
		// rcv() should never block() as we are always using non-blocking sockets
		while ((ret = recv(self->transport->master->fd, buff, sizeof(buff), 0)) > 0){
			TSK_DEBUG_INFO("Flushing RTP Buffer %d", ret);
		}
		TSK_DEBUG_INFO("End flushing RTP socket");
	}

	/* start the transport */
	if((ret = tnet_transport_start(self->transport))){
		TSK_DEBUG_ERROR("Failed to start the RTP/RTCP transport");
		return ret;
	}
	
	/* RTP */
	if((ret = tnet_sockaddr_init(self->rtp.remote_ip, self->rtp.remote_port, self->transport->master->type, &self->rtp.remote_addr))){
		tnet_transport_shutdown(self->transport);
		TSK_DEBUG_ERROR("Invalid RTP host:port [%s:%u]", self->rtp.remote_ip, self->rtp.remote_port);
		return ret;
	}

	/* RTCP */
	if(self->enable_rtcp){
		if(!self->rtcp.remote_ip){
			self->rtcp.remote_ip = tsk_strdup(self->rtp.remote_ip);
		}
		if(!self->rtcp.remote_port){
			self->rtcp.remote_port = self->rtp.remote_port;
		}
	}
	if(self->enable_rtcp && (ret = tnet_sockaddr_init(self->rtcp.remote_ip, self->rtcp.remote_port, self->rtcp.local_socket->type, &self->rtp.remote_addr))){
		TSK_DEBUG_ERROR("Invalid RTCP host:port [%s:%u]", self->rtcp.remote_ip, self->rtcp.remote_port);
		/* do not exit */
	}
	
	/* add RTCP socket to the transport */
	if(self->enable_rtcp && (ret = tnet_transport_add_socket(self->transport, self->rtcp.local_socket->fd, self->rtcp.local_socket->type, tsk_false, tsk_true/* only Meaningful for tls*/))){
		TSK_DEBUG_ERROR("Failed to add RTCP socket");
		/* do not exit */
	}


#if 0
			{
				int flags;
				if((flags = fcntl(self->transport->master->fd, F_GETFL, 0)) < 0) { 
					TNET_PRINT_LAST_ERROR("fcntl(F_GETFL) have failed.");
				}
				else{
					if(fcntl(self->transport->master->fd, F_SETFL, flags | O_DIRECT) < 0){ 
						TNET_PRINT_LAST_ERROR("fcntl(O_DIRECT) have failed.");
					}
				}
			}
#endif

	/*SRTP*/
#if HAVE_SRTP
	{
		const trtp_srtp_ctx_xt* ctx_remote = &self->srtp_contexts[TRTP_SRTP_LINE_IDX_REMOTE][0];
		const trtp_srtp_ctx_xt* ctx_local = &self->srtp_contexts[TRTP_SRTP_LINE_IDX_LOCAL][0];
		
		if(ctx_remote->initialized){
			self->srtp_ctx_neg_remote = ctx_remote;
			if(ctx_local[0].crypto_type == ctx_remote->crypto_type){
				self->srtp_ctx_neg_local = &ctx_local[0];
			}
			else if(ctx_local[1].crypto_type == ctx_remote->crypto_type){
				self->srtp_ctx_neg_local = &ctx_local[1];
			}
		}
		else{
			self->srtp_ctx_neg_local = tsk_null;
			self->srtp_ctx_neg_remote = tsk_null;
		}
	}
#endif /* HAVE_SRTP */

	self->started = tsk_true;

	return 0;
}

/* Encapsulate raw data into RTP packet and send it over the network 
* Very IMPORTANT: For voice packets, the marker bits indicates the beginning of a talkspurt */
int trtp_manager_send_rtp(trtp_manager_t* self, const void* data, tsk_size_t size, uint32_t duration, tsk_bool_t marker, tsk_bool_t last_packet)
{
	trtp_rtp_packet_t* packet;
	int ret = -1;

	if(!self || !self->transport || !data || !size){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}

	if(!self->started || !self->transport->master){
		//--TSK_DEBUG_ERROR("RTP/RTCP manager should be started before trying to send data");
		return -2;
	}
	
	/* create packet with header */
	if(!(packet = trtp_rtp_packet_create(self->rtp.ssrc, self->rtp.seq_num++, self->rtp.timestamp, self->rtp.payload_type, marker))){
		return -3;
	}
	if(last_packet){
		self->rtp.timestamp += duration;
	}

	/* set data */
#if 0
	if((packet->payload.data = tsk_calloc(size, sizeof(uint8_t)))){
		memcpy(packet->payload.data, data, size);
		packet->payload.size = size;
	}
#else
	packet->payload.data_const = data;
	packet->payload.size = size;
#endif

	ret = trtp_manager_send_rtp_2(self, packet);
	TSK_OBJECT_SAFE_FREE(packet);
	return ret;
}

int trtp_manager_send_rtp_2(trtp_manager_t* self, const struct trtp_rtp_packet_s* packet)
{
	tsk_buffer_t* buffer;
	int ret = -2;
	tsk_size_t rtp_buff_pad_count = 0;

#if HAVE_SRTP
	if(self->srtp_ctx_neg_local){
		rtp_buff_pad_count = SRTP_MAX_TRAILER_LEN;
	}
#endif

	if(!self || !packet){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}

	/* serialize and send over the network */
	if((buffer = trtp_rtp_packet_serialize(packet, rtp_buff_pad_count))){
		void* data_ptr = buffer->data;
		int data_size = buffer->size;
#if HAVE_SRTP
		if(self->srtp_ctx_neg_local){
			if(srtp_protect(self->srtp_ctx_neg_local->session, data_ptr, &data_size) != err_status_ok){
				TSK_DEBUG_ERROR("srtp_protect() failed");
				TSK_OBJECT_SAFE_FREE(buffer);
				return -5;
			}
		}
#endif
		if(/* number of bytes sent */tnet_sockfd_sendto(self->transport->master->fd, (const struct sockaddr *)&self->rtp.remote_addr, data_ptr, data_size)){
			ret = 0;
		}
		TSK_OBJECT_SAFE_FREE(buffer);
	}
	else{
		TSK_DEBUG_ERROR("Failed to serialize RTP packet");
		ret = -5;
	}

	return ret;
}

/** Stops the RTP/RTCP manager */
int trtp_manager_stop(trtp_manager_t* self)
{
	int ret;

	if(!self){
		TSK_DEBUG_ERROR("Invalid parameter");
		return -1;
	}

	if(!self->started){
		TSK_DEBUG_WARN("RTP/RTCP manager not started");
		return 0;
	}

	if(!self->transport){
		TSK_DEBUG_ERROR("RTP/RTCP manager not prepared");
		return -2;
	}

	if(!(ret = tnet_transport_shutdown(self->transport))){
		self->started = tsk_false;
	}

	return ret;
}




//=================================================================================================
//	RTP manager object definition
//
static tsk_object_t* trtp_manager_ctor(tsk_object_t * self, va_list * app)
{
	trtp_manager_t *manager = self;
	if(manager){
		manager->port_range.start = TRTP_PORT_RANGE_START;
		manager->port_range.stop = TRTP_PORT_RANGE_STOP;

		/* rtp */
		manager->rtp.timestamp = rand()^rand();
		manager->rtp.seq_num = rand()^rand();
		manager->rtp.ssrc = rand()^rand();

		/* rtcp */
	}
	return self;
}

static tsk_object_t* trtp_manager_dtor(tsk_object_t * self)
{ 
	trtp_manager_t *manager = self;
	if(manager){
		/* stop */
		if(manager->started){
			trtp_manager_stop(manager);
		}

		/* rtp */
		TSK_FREE(manager->rtp.remote_ip);
		TSK_FREE(manager->rtp.public_ip);

		/* rtcp */
		TSK_FREE(manager->rtcp.remote_ip);
		TSK_FREE(manager->rtcp.public_ip);
		TSK_OBJECT_SAFE_FREE(manager->rtcp.local_socket);
		
		/* srtp */
#if HAVE_SRTP
		{
			int i;
			for(i = 0; i < 2; ++i){
				trtp_srtp_ctx_deinit(&manager->srtp_contexts[TRTP_SRTP_LINE_IDX_LOCAL][i]);
				trtp_srtp_ctx_deinit(&manager->srtp_contexts[TRTP_SRTP_LINE_IDX_REMOTE][i]);
			}
		}
#endif

		TSK_FREE(manager->local_ip);
		TSK_OBJECT_SAFE_FREE(manager->transport);
	}

	return self;
}

static const tsk_object_def_t trtp_manager_def_s = 
{
	sizeof(trtp_manager_t),
	trtp_manager_ctor, 
	trtp_manager_dtor,
	tsk_null, 
};
const tsk_object_def_t *trtp_manager_def_t = &trtp_manager_def_s;