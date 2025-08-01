/* SPDX-License-Identifier: GPL-2.0-or-later */
/* AF_RXRPC internal definitions
 *
 * Copyright (C) 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#include <linux/atomic.h>
#include <linux/seqlock.h>
#include <linux/win_minmax.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/sock.h>
#include <net/af_rxrpc.h>
#include <keys/rxrpc-type.h>
#include "protocol.h"

#define FCRYPT_BSIZE 8
struct rxrpc_crypt {
	union {
		u8	x[FCRYPT_BSIZE];
		__be32	n[2];
	};
} __attribute__((aligned(8)));

#define rxrpc_queue_work(WS)	queue_work(rxrpc_workqueue, (WS))
#define rxrpc_queue_delayed_work(WS,D)	\
	queue_delayed_work(rxrpc_workqueue, (WS), (D))

struct key_preparsed_payload;
struct rxrpc_connection;
struct rxrpc_txbuf;
struct rxrpc_txqueue;
struct rxgk_context;

/*
 * Mark applied to socket buffers in skb->mark.  skb->priority is used
 * to pass supplementary information.
 */
enum rxrpc_skb_mark {
	RXRPC_SKB_MARK_PACKET,		/* Received packet */
	RXRPC_SKB_MARK_ERROR,		/* Error notification */
	RXRPC_SKB_MARK_CHALLENGE,	/* Challenge notification */
	RXRPC_SKB_MARK_SERVICE_CONN_SECURED, /* Service connection response has been verified */
	RXRPC_SKB_MARK_REJECT_BUSY,	/* Reject with BUSY */
	RXRPC_SKB_MARK_REJECT_ABORT,	/* Reject with ABORT (code in skb->priority) */
	RXRPC_SKB_MARK_REJECT_CONN_ABORT, /* Reject with connection ABORT (code in skb->priority) */
};

/*
 * sk_state for RxRPC sockets
 */
enum {
	RXRPC_UNBOUND = 0,
	RXRPC_CLIENT_UNBOUND,		/* Unbound socket used as client */
	RXRPC_CLIENT_BOUND,		/* client local address bound */
	RXRPC_SERVER_BOUND,		/* server local address bound */
	RXRPC_SERVER_BOUND2,		/* second server local address bound */
	RXRPC_SERVER_LISTENING,		/* server listening for connections */
	RXRPC_SERVER_LISTEN_DISABLED,	/* server listening disabled */
	RXRPC_CLOSE,			/* socket is being closed */
};

/*
 * Per-network namespace data.
 */
struct rxrpc_net {
	struct proc_dir_entry	*proc_net;	/* Subdir in /proc/net */
	u32			epoch;		/* Local epoch for detecting local-end reset */
	struct list_head	calls;		/* List of calls active in this namespace */
	spinlock_t		call_lock;	/* Lock for ->calls */
	atomic_t		nr_calls;	/* Count of allocated calls */

	atomic_t		nr_conns;
	struct list_head	bundle_proc_list; /* List of bundles for proc */
	struct list_head	conn_proc_list;	/* List of conns in this namespace for proc */
	struct list_head	service_conns;	/* Service conns in this namespace */
	rwlock_t		conn_lock;	/* Lock for ->conn_proc_list, ->service_conns */
	struct work_struct	service_conn_reaper;
	struct timer_list	service_conn_reap_timer;

	bool			live;

	atomic_t		nr_client_conns;

	struct hlist_head	local_endpoints;
	struct mutex		local_mutex;	/* Lock for ->local_endpoints */

	DECLARE_HASHTABLE	(peer_hash, 10);
	spinlock_t		peer_hash_lock;	/* Lock for ->peer_hash */

#define RXRPC_KEEPALIVE_TIME 20 /* NAT keepalive time in seconds */
	u8			peer_keepalive_cursor;
	time64_t		peer_keepalive_base;
	struct list_head	peer_keepalive[32];
	struct list_head	peer_keepalive_new;
	struct timer_list	peer_keepalive_timer;
	struct work_struct	peer_keepalive_work;

	atomic_t		stat_tx_data;
	atomic_t		stat_tx_data_retrans;
	atomic_t		stat_tx_data_send;
	atomic_t		stat_tx_data_send_frag;
	atomic_t		stat_tx_data_send_fail;
	atomic_t		stat_tx_data_send_msgsize;
	atomic_t		stat_tx_data_underflow;
	atomic_t		stat_tx_data_cwnd_reset;
	atomic_t		stat_rx_data;
	atomic_t		stat_rx_data_reqack;
	atomic_t		stat_rx_data_jumbo;

	atomic_t		stat_tx_ack_fill;
	atomic_t		stat_tx_ack_send;
	atomic_t		stat_tx_ack_skip;
	atomic_t		stat_tx_acks[256];
	atomic_t		stat_rx_acks[256];
	atomic_t		stat_tx_jumbo[10];
	atomic_t		stat_rx_jumbo[10];

	atomic_t		stat_why_req_ack[8];

	atomic_t		stat_io_loop;
};

/*
 * Service backlog preallocation.
 *
 * This contains circular buffers of preallocated peers, connections and calls
 * for incoming service calls and their head and tail pointers.  This allows
 * calls to be set up in the data_ready handler, thereby avoiding the need to
 * shuffle packets around so much.
 */
struct rxrpc_backlog {
	unsigned short		peer_backlog_head;
	unsigned short		peer_backlog_tail;
	unsigned short		conn_backlog_head;
	unsigned short		conn_backlog_tail;
	unsigned short		call_backlog_head;
	unsigned short		call_backlog_tail;
#define RXRPC_BACKLOG_MAX	32
	struct rxrpc_peer	*peer_backlog[RXRPC_BACKLOG_MAX];
	struct rxrpc_connection	*conn_backlog[RXRPC_BACKLOG_MAX];
	struct rxrpc_call	*call_backlog[RXRPC_BACKLOG_MAX];
};

/*
 * RxRPC socket definition
 */
struct rxrpc_sock {
	/* WARNING: sk has to be the first member */
	struct sock		sk;
	const struct rxrpc_kernel_ops *app_ops;	/* Table of kernel app notification funcs */
	struct rxrpc_local	*local;		/* local endpoint */
	struct rxrpc_backlog	*backlog;	/* Preallocation for services */
	struct sk_buff_head	recvmsg_oobq;	/* OOB messages for recvmsg to pick up */
	struct rb_root		pending_oobq;	/* OOB messages awaiting userspace to respond to */
	u64			oob_id_counter;	/* OOB message ID counter */
	spinlock_t		incoming_lock;	/* Incoming call vs service shutdown lock */
	struct list_head	sock_calls;	/* List of calls owned by this socket */
	struct list_head	to_be_accepted;	/* calls awaiting acceptance */
	struct list_head	recvmsg_q;	/* Calls awaiting recvmsg's attention  */
	spinlock_t		recvmsg_lock;	/* Lock for recvmsg_q */
	struct key		*key;		/* security for this socket */
	struct key		*securities;	/* list of server security descriptors */
	struct rb_root		calls;		/* User ID -> call mapping */
	unsigned long		flags;
#define RXRPC_SOCK_CONNECTED		0	/* connect_srx is set */
#define RXRPC_SOCK_MANAGE_RESPONSE	1	/* User wants to manage RESPONSE packets */
	rwlock_t		call_lock;	/* lock for calls */
	u32			min_sec_level;	/* minimum security level */
#define RXRPC_SECURITY_MAX	RXRPC_SECURITY_ENCRYPT
	bool			exclusive;	/* Exclusive connection for a client socket */
	u16			second_service;	/* Additional service bound to the endpoint */
	struct {
		/* Service upgrade information */
		u16		from;		/* Service ID to upgrade (if not 0) */
		u16		to;		/* service ID to upgrade to */
	} service_upgrade;
	sa_family_t		family;		/* Protocol family created with */
	struct sockaddr_rxrpc	srx;		/* Primary Service/local addresses */
	struct sockaddr_rxrpc	connect_srx;	/* Default client address from connect() */
};

#define rxrpc_sk(__sk) container_of((__sk), struct rxrpc_sock, sk)

/*
 * CPU-byteorder normalised Rx packet header.
 */
struct rxrpc_host_header {
	u32		epoch;		/* client boot timestamp */
	u32		cid;		/* connection and channel ID */
	u32		callNumber;	/* call ID (0 for connection-level packets) */
	u32		seq;		/* sequence number of pkt in call stream */
	u32		serial;		/* serial number of pkt sent to network */
	u8		type;		/* packet type */
	u8		flags;		/* packet flags */
	u8		userStatus;	/* app-layer defined status */
	u8		securityIndex;	/* security protocol ID */
	union {
		u16	_rsvd;		/* reserved */
		u16	cksum;		/* kerberos security checksum */
	};
	u16		serviceId;	/* service ID */
} __packed;

/*
 * RxRPC socket buffer private variables
 * - max 48 bytes (struct sk_buff::cb)
 */
struct rxrpc_skb_priv {
	union {
		struct rxrpc_connection *poke_conn;	/* Conn referred to (poke packet) */
		struct {
			u16		offset;		/* Offset of data */
			u16		len;		/* Length of data */
			u8		flags;
#define RXRPC_RX_VERIFIED	0x01
		};
		struct {
			rxrpc_seq_t	first_ack;	/* First packet in acks table */
			rxrpc_seq_t	prev_ack;	/* Highest seq seen */
			rxrpc_serial_t	acked_serial;	/* Packet in response to (or 0) */
			u16		nr_acks;	/* Number of acks+nacks */
			u8		reason;		/* Reason for ack */
		} ack;
		struct {
			struct rxrpc_connection *conn;	/* Connection referred to */
			union {
				u32 rxkad_nonce;
			};
		} chall;
		struct {
			rxrpc_serial_t	challenge_serial;
			u32		kvno;
			u32		version;
			u16		len;
			u16		ticket_len;
		} resp;
	};
	struct rxrpc_host_header hdr;	/* RxRPC packet header from this packet */
};

#define rxrpc_skb(__skb) ((struct rxrpc_skb_priv *) &(__skb)->cb)

/*
 * RxRPC security module interface
 */
struct rxrpc_security {
	const char		*name;		/* name of this service */
	u8			security_index;	/* security type provided */
	u32			no_key_abort;	/* Abort code indicating no key */

	/* Initialise a security service */
	int (*init)(void);

	/* Clean up a security service */
	void (*exit)(void);

	/* Parse the information from a server key */
	int (*preparse_server_key)(struct key_preparsed_payload *);

	/* Clean up the preparse buffer after parsing a server key */
	void (*free_preparse_server_key)(struct key_preparsed_payload *);

	/* Destroy the payload of a server key */
	void (*destroy_server_key)(struct key *);

	/* Describe a server key */
	void (*describe_server_key)(const struct key *, struct seq_file *);

	/* initialise a connection's security */
	int (*init_connection_security)(struct rxrpc_connection *,
					struct rxrpc_key_token *);

	/* Work out how much data we can store in a packet, given an estimate
	 * of the amount of data remaining and allocate a data buffer.
	 */
	struct rxrpc_txbuf *(*alloc_txbuf)(struct rxrpc_call *call, size_t remaining, gfp_t gfp);

	/* impose security on a packet */
	int (*secure_packet)(struct rxrpc_call *, struct rxrpc_txbuf *);

	/* verify the security on a received packet */
	int (*verify_packet)(struct rxrpc_call *, struct sk_buff *);

	/* Free crypto request on a call */
	void (*free_call_crypto)(struct rxrpc_call *);

	/* issue a challenge */
	int (*issue_challenge)(struct rxrpc_connection *);

	/* Validate a challenge packet */
	bool (*validate_challenge)(struct rxrpc_connection *conn,
				   struct sk_buff *skb);

	/* Fill out the cmsg for recvmsg() to pass on a challenge to userspace.
	 * The security class gets to add additional information.
	 */
	int (*challenge_to_recvmsg)(struct rxrpc_connection *conn,
				    struct sk_buff *challenge,
				    struct msghdr *msg);

	/* Parse sendmsg() control message and respond to challenge. */
	int (*sendmsg_respond_to_challenge)(struct sk_buff *challenge,
					    struct msghdr *msg);

	/* respond to a challenge */
	int (*respond_to_challenge)(struct rxrpc_connection *conn,
				    struct sk_buff *challenge);

	/* verify a response */
	int (*verify_response)(struct rxrpc_connection *,
			       struct sk_buff *);

	/* clear connection security */
	void (*clear)(struct rxrpc_connection *);

	/* Default ticket -> key decoder */
	int (*default_decode_ticket)(struct rxrpc_connection *conn, struct sk_buff *skb,
				     unsigned int ticket_offset, unsigned int ticket_len,
				     struct key **_key);
};

/*
 * RxRPC local transport endpoint description
 * - owned by a single AF_RXRPC socket
 * - pointed to by transport socket struct sk_user_data
 */
struct rxrpc_local {
	struct rcu_head		rcu;
	atomic_t		active_users;	/* Number of users of the local endpoint */
	refcount_t		ref;		/* Number of references to the structure */
	struct net		*net;		/* The network namespace */
	struct rxrpc_net	*rxnet;		/* Our bits in the network namespace */
	struct hlist_node	link;
	struct socket		*socket;	/* my UDP socket */
	struct task_struct	*io_thread;
	struct completion	io_thread_ready; /* Indication that the I/O thread started */
	struct page_frag_cache	tx_alloc;	/* Tx control packet allocation (I/O thread only) */
	struct rxrpc_sock	*service;	/* Service(s) listening on this endpoint */
#ifdef CONFIG_AF_RXRPC_INJECT_RX_DELAY
	struct sk_buff_head	rx_delay_queue;	/* Delay injection queue */
#endif
	struct sk_buff_head	rx_queue;	/* Received packets */
	struct list_head	conn_attend_q;	/* Conns requiring immediate attention */
	struct list_head	call_attend_q;	/* Calls requiring immediate attention */

	struct rb_root		client_bundles;	/* Client connection bundles by socket params */
	spinlock_t		client_bundles_lock; /* Lock for client_bundles */
	bool			kill_all_client_conns;
	struct list_head	idle_client_conns;
	struct timer_list	client_conn_reap_timer;
	unsigned long		client_conn_flags;
#define RXRPC_CLIENT_CONN_REAP_TIMER	0	/* The client conn reap timer expired */

	spinlock_t		lock;		/* access lock */
	rwlock_t		services_lock;	/* lock for services list */
	int			debug_id;	/* debug ID for printks */
	bool			dead;
	bool			service_closed;	/* Service socket closed */
	struct idr		conn_ids;	/* List of connection IDs */
	struct list_head	new_client_calls; /* Newly created client calls need connection */
	spinlock_t		client_call_lock; /* Lock for ->new_client_calls */
	struct sockaddr_rxrpc	srx;		/* local address */
	union {
		/* Provide a kvec table sufficiently large to manage either a
		 * DATA packet with a maximum set of jumbo subpackets or a PING
		 * ACK padded out to 64K with zeropages for PMTUD.
		 */
		struct kvec		kvec[1 + RXRPC_MAX_NR_JUMBO > 3 + 16 ?
					     1 + RXRPC_MAX_NR_JUMBO : 3 + 16];
		struct bio_vec		bvec[3 + 16];
	};
};

/*
 * RxRPC remote transport endpoint definition
 * - matched by local endpoint, remote port, address and protocol type
 */
struct rxrpc_peer {
	struct rcu_head		rcu;		/* This must be first */
	refcount_t		ref;
	unsigned long		hash_key;
	struct hlist_node	hash_link;
	struct rxrpc_local	*local;
	struct hlist_head	error_targets;	/* targets for net error distribution */
	struct rb_root		service_conns;	/* Service connections */
	struct list_head	keepalive_link;	/* Link in net->peer_keepalive[] */
	unsigned long		app_data;	/* Application data (e.g. afs_server) */
	time64_t		last_tx_at;	/* Last time packet sent here */
	seqlock_t		service_conn_lock;
	spinlock_t		lock;		/* access lock */
	int			debug_id;	/* debug ID for printks */
	struct sockaddr_rxrpc	srx;		/* remote address */

	/* Path MTU discovery [RFC8899] */
	unsigned int		pmtud_trial;	/* Current MTU probe size */
	unsigned int		pmtud_good;	/* Largest working MTU probe we've tried */
	unsigned int		pmtud_bad;	/* Smallest non-working MTU probe we've tried */
	bool			pmtud_lost;	/* T if MTU probe was lost */
	bool			pmtud_probing;	/* T if we have an active probe outstanding */
	bool			pmtud_pending;	/* T if a call to this peer should send a probe */
	u8			pmtud_jumbo;	/* Max jumbo packets for the MTU */
	bool			ackr_adv_pmtud;	/* T if the peer advertises path-MTU */
	unsigned int		ackr_max_data;	/* Maximum data advertised by peer */
	unsigned int		if_mtu;		/* Local interface MTU (- hdrsize) for this peer */
	unsigned int		max_data;	/* Maximum packet data capacity for this peer */
	unsigned short		hdrsize;	/* header size (IP + UDP + RxRPC) */
	unsigned short		tx_seg_max;	/* Maximum number of transmissable segments */

	/* Calculated RTT cache */
	unsigned int		recent_srtt_us;
	unsigned int		recent_rto_us;

	u8			cong_ssthresh;	/* Congestion slow-start threshold */
};

/*
 * Keys for matching a connection.
 */
struct rxrpc_conn_proto {
	union {
		struct {
			u32	epoch;		/* epoch of this connection */
			u32	cid;		/* connection ID */
		};
		u64		index_key;
	};
};

struct rxrpc_conn_parameters {
	struct rxrpc_local	*local;		/* Representation of local endpoint */
	struct rxrpc_peer	*peer;		/* Representation of remote endpoint */
	struct key		*key;		/* Security details */
	bool			exclusive;	/* T if conn is exclusive */
	bool			upgrade;	/* T if service ID can be upgraded */
	u16			service_id;	/* Service ID for this connection */
	u32			security_level;	/* Security level selected */
};

/*
 * Call completion condition (state == RXRPC_CALL_COMPLETE).
 */
enum rxrpc_call_completion {
	RXRPC_CALL_SUCCEEDED,		/* - Normal termination */
	RXRPC_CALL_REMOTELY_ABORTED,	/* - call aborted by peer */
	RXRPC_CALL_LOCALLY_ABORTED,	/* - call aborted locally on error or close */
	RXRPC_CALL_LOCAL_ERROR,		/* - call failed due to local error */
	RXRPC_CALL_NETWORK_ERROR,	/* - call terminated by network error */
	NR__RXRPC_CALL_COMPLETIONS
};

/*
 * Bits in the connection flags.
 */
enum rxrpc_conn_flag {
	RXRPC_CONN_IN_SERVICE_CONNS,	/* Conn is in peer->service_conns */
	RXRPC_CONN_DONT_REUSE,		/* Don't reuse this connection */
	RXRPC_CONN_PROBING_FOR_UPGRADE,	/* Probing for service upgrade */
	RXRPC_CONN_FINAL_ACK_0,		/* Need final ACK for channel 0 */
	RXRPC_CONN_FINAL_ACK_1,		/* Need final ACK for channel 1 */
	RXRPC_CONN_FINAL_ACK_2,		/* Need final ACK for channel 2 */
	RXRPC_CONN_FINAL_ACK_3,		/* Need final ACK for channel 3 */
};

#define RXRPC_CONN_FINAL_ACK_MASK ((1UL << RXRPC_CONN_FINAL_ACK_0) |	\
				   (1UL << RXRPC_CONN_FINAL_ACK_1) |	\
				   (1UL << RXRPC_CONN_FINAL_ACK_2) |	\
				   (1UL << RXRPC_CONN_FINAL_ACK_3))

/*
 * Events that can be raised upon a connection.
 */
enum rxrpc_conn_event {
	RXRPC_CONN_EV_CHALLENGE,	/* Send challenge packet */
	RXRPC_CONN_EV_ABORT_CALLS,	/* Abort attached calls */
};

/*
 * The connection protocol state.
 */
enum rxrpc_conn_proto_state {
	RXRPC_CONN_UNUSED,		/* Connection not yet attempted */
	RXRPC_CONN_CLIENT_UNSECURED,	/* Client connection needs security init */
	RXRPC_CONN_CLIENT,		/* Client connection */
	RXRPC_CONN_SERVICE_PREALLOC,	/* Service connection preallocation */
	RXRPC_CONN_SERVICE_UNSECURED,	/* Service unsecured connection */
	RXRPC_CONN_SERVICE_CHALLENGING,	/* Service challenging for security */
	RXRPC_CONN_SERVICE,		/* Service secured connection */
	RXRPC_CONN_ABORTED,		/* Conn aborted */
	RXRPC_CONN__NR_STATES
};

/*
 * RxRPC client connection bundle.
 */
struct rxrpc_bundle {
	struct rxrpc_local	*local;		/* Representation of local endpoint */
	struct rxrpc_peer	*peer;		/* Remote endpoint */
	struct key		*key;		/* Security details */
	struct list_head	proc_link;	/* Link in net->bundle_proc_list */
	const struct rxrpc_security *security;	/* applied security module */
	refcount_t		ref;
	atomic_t		active;		/* Number of active users */
	unsigned int		debug_id;
	u32			security_level;	/* Security level selected */
	u16			service_id;	/* Service ID for this connection */
	bool			try_upgrade;	/* True if the bundle is attempting upgrade */
	bool			exclusive;	/* T if conn is exclusive */
	bool			upgrade;	/* T if service ID can be upgraded */
	unsigned short		alloc_error;	/* Error from last conn allocation */
	struct rb_node		local_node;	/* Node in local->client_conns */
	struct list_head	waiting_calls;	/* Calls waiting for channels */
	unsigned long		avail_chans;	/* Mask of available channels */
	unsigned int		conn_ids[4];	/* Connection IDs. */
	struct rxrpc_connection	*conns[4];	/* The connections in the bundle (max 4) */
};

/*
 * RxRPC connection definition
 * - matched by { local, peer, epoch, conn_id, direction }
 * - each connection can only handle four simultaneous calls
 */
struct rxrpc_connection {
	struct rxrpc_conn_proto	proto;
	struct rxrpc_local	*local;		/* Representation of local endpoint */
	struct rxrpc_peer	*peer;		/* Remote endpoint */
	struct rxrpc_net	*rxnet;		/* Network namespace to which call belongs */
	struct key		*key;		/* Security details */
	struct list_head	attend_link;	/* Link in local->conn_attend_q */

	refcount_t		ref;
	atomic_t		active;		/* Active count for service conns */
	struct rcu_head		rcu;
	struct list_head	cache_link;

	unsigned char		act_chans;	/* Mask of active channels */
	struct rxrpc_channel {
		unsigned long		final_ack_at;	/* Time at which to issue final ACK */
		struct rxrpc_call	*call;		/* Active call */
		unsigned int		call_debug_id;	/* call->debug_id */
		u32			call_id;	/* ID of current call */
		u32			call_counter;	/* Call ID counter */
		u32			last_call;	/* ID of last call */
		u8			last_type;	/* Type of last packet */
		union {
			u32		last_seq;
			u32		last_abort;
		};
	} channels[RXRPC_MAXCALLS];

	struct timer_list	timer;		/* Conn event timer */
	struct work_struct	processor;	/* connection event processor */
	struct work_struct	destructor;	/* In-process-context destroyer */
	struct rxrpc_bundle	*bundle;	/* Client connection bundle */
	struct rb_node		service_node;	/* Node in peer->service_conns */
	struct list_head	proc_link;	/* link in procfs list */
	struct list_head	link;		/* link in master connection list */
	struct sk_buff_head	rx_queue;	/* received conn-level packets */
	struct page_frag_cache	tx_data_alloc;	/* Tx DATA packet allocation */
	struct mutex		tx_data_alloc_lock;

	struct mutex		security_lock;	/* Lock for security management */
	const struct rxrpc_security *security;	/* applied security module */
	union {
		struct {
			struct crypto_sync_skcipher *cipher;	/* encryption handle */
			struct rxrpc_crypt csum_iv;	/* packet checksum base */
			u32	nonce;		/* response re-use preventer */
		} rxkad;
		struct {
			struct rxgk_context *keys[4]; /* (Re-)keying buffer */
			u64	start_time;	/* The start time for TK derivation */
			u8	nonce[20];	/* Response re-use preventer */
			u32	enctype;	/* Kerberos 5 encoding type */
			u32	key_number;	/* Current key number */
		} rxgk;
	};
	rwlock_t		security_use_lock; /* Security use/modification lock */
	struct sk_buff		*tx_response;	/* Response packet to be transmitted */

	unsigned long		flags;
	unsigned long		events;
	unsigned long		idle_timestamp;	/* Time at which last became idle */
	spinlock_t		state_lock;	/* state-change lock */
	enum rxrpc_conn_proto_state state;	/* current state of connection */
	enum rxrpc_call_completion completion;	/* Completion condition */
	s32			abort_code;	/* Abort code of connection abort */
	int			debug_id;	/* debug ID for printks */
	rxrpc_serial_t		tx_serial;	/* Outgoing packet serial number counter */
	unsigned int		hi_serial;	/* highest serial number received */
	rxrpc_serial_t		pmtud_probe;	/* Serial of MTU probe (or 0) */
	unsigned int		pmtud_call;	/* ID of call used for probe */
	u32			service_id;	/* Service ID, possibly upgraded */
	u32			security_level;	/* Security level selected */
	u8			security_ix;	/* security type */
	u8			out_clientflag;	/* RXRPC_CLIENT_INITIATED if we are client */
	u8			bundle_shift;	/* Index into bundle->avail_chans */
	bool			exclusive;	/* T if conn is exclusive */
	bool			upgrade;	/* T if service ID can be upgraded */
	u16			orig_service_id; /* Originally requested service ID */
	short			error;		/* Local error code */
};

static inline bool rxrpc_to_server(const struct rxrpc_skb_priv *sp)
{
	return sp->hdr.flags & RXRPC_CLIENT_INITIATED;
}

static inline bool rxrpc_to_client(const struct rxrpc_skb_priv *sp)
{
	return !rxrpc_to_server(sp);
}

/*
 * Flags in call->flags.
 */
enum rxrpc_call_flag {
	RXRPC_CALL_RELEASED,		/* call has been released - no more message to userspace */
	RXRPC_CALL_HAS_USERID,		/* has a user ID attached */
	RXRPC_CALL_IS_SERVICE,		/* Call is service call */
	RXRPC_CALL_EXPOSED,		/* The call was exposed to the world */
	RXRPC_CALL_RX_LAST,		/* Received the last packet (at rxtx_top) */
	RXRPC_CALL_TX_LAST,		/* Last packet in Tx buffer (at rxtx_top) */
	RXRPC_CALL_TX_ALL_ACKED,	/* Last packet has been hard-acked */
	RXRPC_CALL_TX_NO_MORE,		/* No more data to transmit (MSG_MORE deasserted) */
	RXRPC_CALL_SEND_PING,		/* A ping will need to be sent */
	RXRPC_CALL_RETRANS_TIMEOUT,	/* Retransmission due to timeout occurred */
	RXRPC_CALL_BEGAN_RX_TIMER,	/* We began the expect_rx_by timer */
	RXRPC_CALL_RX_HEARD,		/* The peer responded at least once to this call */
	RXRPC_CALL_DISCONNECTED,	/* The call has been disconnected */
	RXRPC_CALL_KERNEL,		/* The call was made by the kernel */
	RXRPC_CALL_UPGRADE,		/* Service upgrade was requested for the call */
	RXRPC_CALL_EXCLUSIVE,		/* The call uses a once-only connection */
	RXRPC_CALL_RX_IS_IDLE,		/* recvmsg() is idle - send an ACK */
	RXRPC_CALL_RECVMSG_READ_ALL,	/* recvmsg() read all of the received data */
	RXRPC_CALL_CONN_CHALLENGING,	/* The connection is being challenged */
};

/*
 * Events that can be raised on a call.
 */
enum rxrpc_call_event {
	RXRPC_CALL_EV_ACK_LOST,		/* ACK may be lost, send ping */
	RXRPC_CALL_EV_INITIAL_PING,	/* Send initial ping for a new service call */
};

/*
 * The states that a call can be in.
 */
enum rxrpc_call_state {
	RXRPC_CALL_UNINITIALISED,
	RXRPC_CALL_CLIENT_AWAIT_CONN,	/* - client waiting for connection to become available */
	RXRPC_CALL_CLIENT_SEND_REQUEST,	/* - client sending request phase */
	RXRPC_CALL_CLIENT_AWAIT_REPLY,	/* - client awaiting reply */
	RXRPC_CALL_CLIENT_RECV_REPLY,	/* - client receiving reply phase */
	RXRPC_CALL_SERVER_PREALLOC,	/* - service preallocation */
	RXRPC_CALL_SERVER_RECV_REQUEST,	/* - server receiving request */
	RXRPC_CALL_SERVER_ACK_REQUEST,	/* - server pending ACK of request */
	RXRPC_CALL_SERVER_SEND_REPLY,	/* - server sending reply */
	RXRPC_CALL_SERVER_AWAIT_ACK,	/* - server awaiting final ACK */
	RXRPC_CALL_COMPLETE,		/* - call complete */
	NR__RXRPC_CALL_STATES
};

/*
 * Call Tx congestion management modes.
 */
enum rxrpc_ca_state {
	RXRPC_CA_SLOW_START,
	RXRPC_CA_CONGEST_AVOIDANCE,
	RXRPC_CA_PACKET_LOSS,
	RXRPC_CA_FAST_RETRANSMIT,
	NR__RXRPC_CA_STATES
} __mode(byte);

/*
 * Current purpose of call RACK timer.  According to the RACK-TLP protocol
 * [RFC8985], the transmission timer (call->rack_timo_at) may only be used for
 * one of these at once.
 */
enum rxrpc_rack_timer_mode {
	RXRPC_CALL_RACKTIMER_OFF,		/* Timer not running */
	RXRPC_CALL_RACKTIMER_RACK_REORDER,	/* RACK reordering timer */
	RXRPC_CALL_RACKTIMER_TLP_PTO,		/* TLP timeout */
	RXRPC_CALL_RACKTIMER_RTO,		/* Retransmission timeout */
} __mode(byte);

/*
 * RxRPC call definition
 * - matched by { connection, call_id }
 */
struct rxrpc_call {
	struct rcu_head		rcu;
	struct rxrpc_connection	*conn;		/* connection carrying call */
	struct rxrpc_bundle	*bundle;	/* Connection bundle to use */
	struct rxrpc_peer	*peer;		/* Peer record for remote address */
	struct rxrpc_local	*local;		/* Representation of local endpoint */
	struct rxrpc_sock __rcu	*socket;	/* socket responsible */
	struct rxrpc_net	*rxnet;		/* Network namespace to which call belongs */
	struct key		*key;		/* Security details */
	const struct rxrpc_security *security;	/* applied security module */
	struct mutex		user_mutex;	/* User access mutex */
	struct sockaddr_rxrpc	dest_srx;	/* Destination address */
	ktime_t			delay_ack_at;	/* When DELAY ACK needs to happen */
	ktime_t			rack_timo_at;	/* When ACK is figured as lost */
	ktime_t			ping_at;	/* When next to send a ping */
	ktime_t			keepalive_at;	/* When next to send a keepalive ping */
	ktime_t			expect_rx_by;	/* When we expect to get a packet by */
	ktime_t			expect_req_by;	/* When we expect to get a request DATA packet by */
	ktime_t			expect_term_by;	/* When we expect call termination by */
	u32			next_rx_timo;	/* Timeout for next Rx packet (ms) */
	u32			next_req_timo;	/* Timeout for next Rx request packet (ms) */
	u32			hard_timo;	/* Maximum lifetime or 0 (s) */
	struct timer_list	timer;		/* Combined event timer */
	struct work_struct	destroyer;	/* In-process-context destroyer */
	rxrpc_notify_rx_t	notify_rx;	/* kernel service Rx notification function */
	struct list_head	link;		/* link in master call list */
	struct list_head	wait_link;	/* Link in local->new_client_calls */
	struct hlist_node	error_link;	/* link in error distribution list */
	struct list_head	accept_link;	/* Link in rx->acceptq */
	struct list_head	recvmsg_link;	/* Link in rx->recvmsg_q */
	struct list_head	sock_link;	/* Link in rx->sock_calls */
	struct rb_node		sock_node;	/* Node in rx->calls */
	struct list_head	attend_link;	/* Link in local->call_attend_q */
	struct rxrpc_txbuf	*tx_pending;	/* Tx buffer being filled */
	wait_queue_head_t	waitq;		/* Wait queue for channel or Tx */
	s64			tx_total_len;	/* Total length left to be transmitted (or -1) */
	unsigned long		user_call_ID;	/* user-defined call ID */
	unsigned long		flags;
	unsigned long		events;
	spinlock_t		notify_lock;	/* Kernel notification lock */
	unsigned int		send_abort_why; /* Why the abort [enum rxrpc_abort_reason] */
	s32			send_abort;	/* Abort code to be sent */
	short			send_abort_err;	/* Error to be associated with the abort */
	rxrpc_seq_t		send_abort_seq;	/* DATA packet that incurred the abort (or 0) */
	s32			abort_code;	/* Local/remote abort code */
	int			error;		/* Local error incurred */
	enum rxrpc_call_state	_state;		/* Current state of call (needs barrier) */
	enum rxrpc_call_completion completion;	/* Call completion condition */
	refcount_t		ref;
	u8			security_ix;	/* Security type */
	enum rxrpc_interruptibility interruptibility; /* At what point call may be interrupted */
	u32			call_id;	/* call ID on connection  */
	u32			cid;		/* connection ID plus channel index */
	u32			security_level;	/* Security level selected */
	u32			security_enctype; /* Security-specific encoding type (or 0) */
	int			debug_id;	/* debug ID for printks */
	unsigned short		rx_pkt_offset;	/* Current recvmsg packet offset */
	unsigned short		rx_pkt_len;	/* Current recvmsg packet len */

	/* Sendmsg data tracking. */
	rxrpc_seq_t		send_top;	/* Highest Tx slot filled by sendmsg. */
	struct rxrpc_txqueue	*send_queue;	/* Queue that sendmsg is writing into */

	/* Transmitted data tracking. */
	struct rxrpc_txqueue	*tx_queue;	/* Start of transmission buffers */
	struct rxrpc_txqueue	*tx_qtail;	/* End of transmission buffers */
	rxrpc_seq_t		tx_qbase;	/* First slot in tx_queue */
	rxrpc_seq_t		tx_bottom;	/* First packet in buffer */
	rxrpc_seq_t		tx_transmitted;	/* Highest packet transmitted */
	rxrpc_seq_t		tx_top;		/* Highest Tx slot allocated. */
	rxrpc_serial_t		tx_last_serial;	/* Serial of last DATA transmitted */
	u16			tx_backoff;	/* Delay to insert due to Tx failure (ms) */
	u16			tx_nr_sent;	/* Number of packets sent, but unacked */
	u16			tx_nr_lost;	/* Number of packets marked lost */
	u16			tx_nr_resent;	/* Number of packets resent, but unacked */
	u16			tx_winsize;	/* Maximum size of Tx window */
#define RXRPC_TX_MAX_WINDOW	128
	u8			tx_jumbo_max;	/* Maximum subpkts peer will accept */
	ktime_t			tx_last_sent;	/* Last time a transmission occurred */

	/* Received data tracking */
	struct sk_buff_head	recvmsg_queue;	/* Queue of packets ready for recvmsg() */
	struct sk_buff_head	rx_queue;	/* Queue of packets for this call to receive */
	struct sk_buff_head	rx_oos_queue;	/* Queue of out of sequence packets */

	rxrpc_seq_t		rx_highest_seq;	/* Higest sequence number received */
	rxrpc_seq_t		rx_consumed;	/* Highest packet consumed */
	rxrpc_serial_t		rx_serial;	/* Highest serial received for this call */
	u8			rx_winsize;	/* Size of Rx window */

	/* TCP-style slow-start congestion control [RFC5681].  Since the SMSS
	 * is fixed, we keep these numbers in terms of segments (ie. DATA
	 * packets) rather than bytes.
	 */
#define RXRPC_TX_SMSS		RXRPC_JUMBO_DATALEN
#define RXRPC_MIN_CWND		4
	enum rxrpc_ca_state	cong_ca_state;	/* Congestion control state */
	u8			cong_extra;	/* Extra to send for congestion management */
	u16			cong_cwnd;	/* Congestion window size */
	u16			cong_ssthresh;	/* Slow-start threshold */
	u16			cong_dup_acks;	/* Count of ACKs showing missing packets */
	u16			cong_cumul_acks; /* Cumulative ACK count */
	ktime_t			cong_tstamp;	/* Last time cwnd was changed */

	/* RACK-TLP [RFC8985] state. */
	ktime_t			rack_xmit_ts;	/* Latest transmission timestamp */
	ktime_t			rack_rtt;	/* RTT of most recently ACK'd segment */
	ktime_t			rack_rtt_ts;	/* Timestamp of rack_rtt */
	ktime_t			rack_reo_wnd;	/* Reordering window */
	unsigned int		rack_reo_wnd_mult; /* Multiplier applied to rack_reo_wnd */
	int			rack_reo_wnd_persist; /* Num loss recoveries before reset reo_wnd */
	rxrpc_seq_t		rack_fack;	/* Highest sequence so far ACK'd */
	rxrpc_seq_t		rack_end_seq;	/* Highest sequence seen */
	rxrpc_seq_t		rack_dsack_round; /* DSACK opt recv'd in latest roundtrip */
	bool			rack_dsack_round_none; /* T if dsack_round is "None" */
	bool			rack_reordering_seen; /* T if detected reordering event */
	enum rxrpc_rack_timer_mode rack_timer_mode; /* Current mode of RACK timer */
	bool			tlp_is_retrans;	/* T if unacked TLP retransmission */
	rxrpc_serial_t		tlp_serial;	/* Serial of TLP probe (or 0 if none in progress) */
	rxrpc_seq_t		tlp_seq;	/* Sequence of TLP probe */
	unsigned int		tlp_rtt_taken;	/* Last time RTT taken */
	ktime_t			tlp_max_ack_delay; /* Sender budget for max delayed ACK interval */

	/* Receive-phase ACK management (ACKs we send). */
	u8			ackr_reason;	/* reason to ACK */
	u16			ackr_sack_base;	/* Starting slot in SACK table ring */
	rxrpc_seq_t		ackr_window;	/* Base of SACK window */
	rxrpc_seq_t		ackr_wtop;	/* Base of SACK window */
	unsigned int		ackr_nr_unacked; /* Number of unacked packets */
	atomic_t		ackr_nr_consumed; /* Number of packets needing hard ACK */
	struct {
#define RXRPC_SACK_SIZE 256
		 /* SACK table for soft-acked packets */
		u8		ackr_sack_table[RXRPC_SACK_SIZE];
	} __aligned(8);

	/* RTT management */
	rxrpc_serial_t		rtt_serial[4];	/* Serial number of DATA or PING sent */
	ktime_t			rtt_sent_at[4];	/* Time packet sent */
	unsigned long		rtt_avail;	/* Mask of available slots in bits 0-3,
						 * Mask of pending samples in 8-11 */
#define RXRPC_CALL_RTT_AVAIL_MASK	0xf
#define RXRPC_CALL_RTT_PEND_SHIFT	8

	/* Transmission-phase ACK management (ACKs we've received). */
	ktime_t			acks_latest_ts;	/* Timestamp of latest ACK received */
	rxrpc_seq_t		acks_hard_ack;	/* Highest sequence hard acked */
	rxrpc_seq_t		acks_prev_seq;	/* Highest previousPacket received */
	rxrpc_seq_t		acks_lowest_nak; /* Lowest NACK in the buffer (or ==tx_hard_ack) */
	rxrpc_serial_t		acks_highest_serial; /* Highest serial number ACK'd */
	unsigned short		acks_nr_sacks;	/* Number of soft acks recorded */
	unsigned short		acks_nr_snacks;	/* Number of soft nacks recorded */

	/* Calculated RTT cache */
	ktime_t			rtt_last_req;	/* Time of last RTT request */
	unsigned int		rtt_count;	/* Number of samples we've got */
	unsigned int		rtt_taken;	/* Number of samples taken (wrapping) */
	struct minmax		min_rtt;	/* Estimated minimum RTT */
	u32			srtt_us;	/* smoothed round trip time << 3 in usecs */
	u32			mdev_us;	/* medium deviation			*/
	u32			mdev_max_us;	/* maximal mdev for the last rtt period	*/
	u32			rttvar_us;	/* smoothed mdev_max			*/
	u32			rto_us;		/* Retransmission timeout in usec */
	u8			backoff;	/* Backoff timeout (as shift) */
};

/*
 * Summary of a new ACK and the changes it made to the Tx buffer packet states.
 */
struct rxrpc_ack_summary {
	rxrpc_serial_t	ack_serial;		/* Serial number of ACK */
	rxrpc_serial_t	acked_serial;		/* Serial number ACK'd */
	u16		in_flight;		/* Number of unreceived transmissions */
	u16		nr_new_hacks;		/* Number of rotated new ACKs */
	u16		nr_new_sacks;		/* Number of new soft ACKs in packet */
	u16		nr_new_snacks;		/* Number of new soft nacks in packet */
	u8		ack_reason;
	bool		new_low_snack:1;	/* T if new low soft NACK found */
	bool		retrans_timeo:1;	/* T if reTx due to timeout happened */
	bool		need_retransmit:1;	/* T if we need transmission */
	bool		rtt_sample_avail:1;	/* T if RTT sample available */
	bool		in_fast_or_rto_recovery:1;
	bool		exiting_fast_or_rto_recovery:1;
	bool		tlp_probe_acked:1;	/* T if the TLP probe seq was acked */
	u8 /*enum rxrpc_congest_change*/ change;
};

/*
 * sendmsg() cmsg-specified parameters.
 */
enum rxrpc_command {
	RXRPC_CMD_SEND_DATA,		/* send data message */
	RXRPC_CMD_SEND_ABORT,		/* request abort generation */
	RXRPC_CMD_REJECT_BUSY,		/* [server] reject a call as busy */
	RXRPC_CMD_CHARGE_ACCEPT,	/* [server] charge accept preallocation */
};

struct rxrpc_call_params {
	s64			tx_total_len;	/* Total Tx data length (if send data) */
	unsigned long		user_call_ID;	/* User's call ID */
	struct {
		u32		hard;		/* Maximum lifetime (sec) */
		u32		idle;		/* Max time since last data packet (msec) */
		u32		normal;		/* Max time since last call packet (msec) */
	} timeouts;
	u8			nr_timeouts;	/* Number of timeouts specified */
	bool			kernel;		/* T if kernel is making the call */
	enum rxrpc_interruptibility interruptibility; /* How is interruptible is the call? */
};

struct rxrpc_send_params {
	struct rxrpc_call_params call;
	u32			abort_code;	/* Abort code to Tx (if abort) */
	enum rxrpc_command	command : 8;	/* The command to implement */
	bool			exclusive;	/* Shared or exclusive call */
	bool			upgrade;	/* If the connection is upgradeable */
};

/*
 * Buffer of data to be output as a packet.
 */
struct rxrpc_txbuf {
	refcount_t		ref;
	rxrpc_seq_t		seq;		/* Sequence number of this packet */
	rxrpc_serial_t		serial;		/* Last serial number transmitted with */
	unsigned int		call_debug_id;
	unsigned int		debug_id;
	unsigned short		len;		/* Amount of data in buffer */
	unsigned short		space;		/* Remaining data space */
	unsigned short		offset;		/* Offset of fill point */
	unsigned short		crypto_header;	/* Size of crypto header */
	unsigned short		sec_header;	/* Size of security header */
	unsigned short		pkt_len;	/* Size of packet content */
	unsigned short		alloc_size;	/* Amount of bufferage allocated */
	unsigned int		flags;
#define RXRPC_TXBUF_WIRE_FLAGS	0xff		/* The wire protocol flags */
#define RXRPC_TXBUF_RESENT	0x100		/* Set if has been resent */
	__be16			cksum;		/* Checksum to go in header */
	bool			jumboable;	/* Can be non-terminal jumbo subpacket */
	void			*data;		/* Data with preceding jumbo header */
};

static inline bool rxrpc_sending_to_server(const struct rxrpc_txbuf *txb)
{
	return txb->flags & RXRPC_CLIENT_INITIATED;
}

static inline bool rxrpc_sending_to_client(const struct rxrpc_txbuf *txb)
{
	return !rxrpc_sending_to_server(txb);
}

/*
 * Transmit queue element, including RACK [RFC8985] per-segment metadata.  The
 * transmission timestamp is in usec from the base.
 */
struct rxrpc_txqueue {
	/* Start with the members we want to prefetch. */
	struct rxrpc_txqueue	*next;
	ktime_t			xmit_ts_base;
	rxrpc_seq_t		qbase;
	u8			nr_reported_acks; /* Number of segments explicitly acked/nacked */
	unsigned long		segment_acked;	/* Bit-per-buf: Set if ACK'd */
	unsigned long		segment_lost;	/* Bit-per-buf: Set if declared lost */
	unsigned long		segment_retransmitted; /* Bit-per-buf: Set if retransmitted */
	unsigned long		rtt_samples;	/* Bit-per-buf: Set if available for RTT */
	unsigned long		ever_retransmitted; /* Bit-per-buf: Set if ever retransmitted */

	/* The arrays we want to pack into as few cache lines as possible. */
	struct {
#define RXRPC_NR_TXQUEUE BITS_PER_LONG
#define RXRPC_TXQ_MASK (RXRPC_NR_TXQUEUE - 1)
		struct rxrpc_txbuf *bufs[RXRPC_NR_TXQUEUE];
		unsigned int	segment_serial[RXRPC_NR_TXQUEUE];
		unsigned int	segment_xmit_ts[RXRPC_NR_TXQUEUE];
	} ____cacheline_aligned;
};

/*
 * Data transmission request.
 */
struct rxrpc_send_data_req {
	ktime_t			now;		/* Current time */
	struct rxrpc_txqueue	*tq;		/* Tx queue segment holding first DATA */
	rxrpc_seq_t		seq;		/* Sequence of first data */
	int			n;		/* Number of DATA packets to glue into jumbo */
	bool			retrans;	/* T if this is a retransmission */
	bool			did_send;	/* T if did actually send */
	bool			tlp_probe;	/* T if this is a TLP probe */
	int /* enum rxrpc_txdata_trace */ trace;
};

#include <trace/events/rxrpc.h>

/*
 * Allocate the next serial number on a connection.  0 must be skipped.
 */
static inline rxrpc_serial_t rxrpc_get_next_serial(struct rxrpc_connection *conn)
{
	rxrpc_serial_t serial;

	serial = conn->tx_serial;
	if (serial == 0)
		serial = 1;
	conn->tx_serial = serial + 1;
	return serial;
}

/*
 * Allocate the next serial n numbers on a connection.  0 must be skipped.
 */
static inline rxrpc_serial_t rxrpc_get_next_serials(struct rxrpc_connection *conn,
						    unsigned int n)
{
	rxrpc_serial_t serial;

	serial = conn->tx_serial;
	if (serial + n <= n)
		serial = 1;
	conn->tx_serial = serial + n;
	return serial;
}

/*
 * af_rxrpc.c
 */
extern atomic_t rxrpc_n_rx_skbs;
extern struct workqueue_struct *rxrpc_workqueue;

/*
 * call_accept.c
 */
int rxrpc_service_prealloc(struct rxrpc_sock *, gfp_t);
void rxrpc_discard_prealloc(struct rxrpc_sock *);
bool rxrpc_new_incoming_call(struct rxrpc_local *local,
			     struct rxrpc_peer *peer,
			     struct rxrpc_connection *conn,
			     struct sockaddr_rxrpc *peer_srx,
			     struct sk_buff *skb);
int rxrpc_user_charge_accept(struct rxrpc_sock *, unsigned long);

/*
 * call_event.c
 */
void rxrpc_propose_ping(struct rxrpc_call *call, u32 serial,
			enum rxrpc_propose_ack_trace why);
void rxrpc_propose_delay_ACK(struct rxrpc_call *, rxrpc_serial_t,
			     enum rxrpc_propose_ack_trace);
void rxrpc_resend_tlp(struct rxrpc_call *call);
void rxrpc_transmit_some_data(struct rxrpc_call *call, unsigned int limit,
			      enum rxrpc_txdata_trace trace);
bool rxrpc_input_call_event(struct rxrpc_call *call);

/*
 * call_object.c
 */
extern const char *const rxrpc_call_states[];
extern const char *const rxrpc_call_completions[];
extern struct kmem_cache *rxrpc_call_jar;

void rxrpc_poke_call(struct rxrpc_call *call, enum rxrpc_call_poke_trace what);
struct rxrpc_call *rxrpc_find_call_by_user_ID(struct rxrpc_sock *, unsigned long);
struct rxrpc_call *rxrpc_alloc_call(struct rxrpc_sock *, gfp_t, unsigned int);
struct rxrpc_call *rxrpc_new_client_call(struct rxrpc_sock *,
					 struct rxrpc_conn_parameters *,
					 struct rxrpc_call_params *, gfp_t,
					 unsigned int)
	__releases(&rx->sk.sk_lock)
	__acquires(&call->user_mutex);
void rxrpc_start_call_timer(struct rxrpc_call *call);
void rxrpc_incoming_call(struct rxrpc_sock *, struct rxrpc_call *,
			 struct sk_buff *);
void rxrpc_release_call(struct rxrpc_sock *, struct rxrpc_call *);
void rxrpc_release_calls_on_socket(struct rxrpc_sock *);
void rxrpc_see_call(struct rxrpc_call *, enum rxrpc_call_trace);
struct rxrpc_call *rxrpc_try_get_call(struct rxrpc_call *, enum rxrpc_call_trace);
void rxrpc_get_call(struct rxrpc_call *, enum rxrpc_call_trace);
void rxrpc_put_call(struct rxrpc_call *, enum rxrpc_call_trace);
void rxrpc_cleanup_call(struct rxrpc_call *);
void rxrpc_destroy_all_calls(struct rxrpc_net *);

static inline bool rxrpc_is_service_call(const struct rxrpc_call *call)
{
	return test_bit(RXRPC_CALL_IS_SERVICE, &call->flags);
}

static inline bool rxrpc_is_client_call(const struct rxrpc_call *call)
{
	return !rxrpc_is_service_call(call);
}

/*
 * call_state.c
 */
bool rxrpc_set_call_completion(struct rxrpc_call *call,
			       enum rxrpc_call_completion compl,
			       u32 abort_code,
			       int error);
bool rxrpc_call_completed(struct rxrpc_call *call);
bool rxrpc_abort_call(struct rxrpc_call *call, rxrpc_seq_t seq,
		      u32 abort_code, int error, enum rxrpc_abort_reason why);
void rxrpc_prefail_call(struct rxrpc_call *call, enum rxrpc_call_completion compl,
			int error);

static inline void rxrpc_set_call_state(struct rxrpc_call *call,
					enum rxrpc_call_state state)
{
	/* Order write of completion info before write of ->state. */
	smp_store_release(&call->_state, state);
	wake_up(&call->waitq);
}

static inline enum rxrpc_call_state __rxrpc_call_state(const struct rxrpc_call *call)
{
	return call->_state; /* Only inside I/O thread */
}

static inline bool __rxrpc_call_is_complete(const struct rxrpc_call *call)
{
	return __rxrpc_call_state(call) == RXRPC_CALL_COMPLETE;
}

static inline enum rxrpc_call_state rxrpc_call_state(const struct rxrpc_call *call)
{
	/* Order read ->state before read of completion info. */
	return smp_load_acquire(&call->_state);
}

static inline bool rxrpc_call_is_complete(const struct rxrpc_call *call)
{
	return rxrpc_call_state(call) == RXRPC_CALL_COMPLETE;
}

static inline bool rxrpc_call_has_failed(const struct rxrpc_call *call)
{
	return rxrpc_call_is_complete(call) && call->completion != RXRPC_CALL_SUCCEEDED;
}

/*
 * conn_client.c
 */
extern unsigned int rxrpc_reap_client_connections;
extern unsigned long rxrpc_conn_idle_client_expiry;
extern unsigned long rxrpc_conn_idle_client_fast_expiry;

void rxrpc_purge_client_connections(struct rxrpc_local *local);
struct rxrpc_bundle *rxrpc_get_bundle(struct rxrpc_bundle *, enum rxrpc_bundle_trace);
void rxrpc_put_bundle(struct rxrpc_bundle *, enum rxrpc_bundle_trace);
int rxrpc_look_up_bundle(struct rxrpc_call *call, gfp_t gfp);
void rxrpc_connect_client_calls(struct rxrpc_local *local);
void rxrpc_expose_client_call(struct rxrpc_call *);
void rxrpc_disconnect_client_call(struct rxrpc_bundle *, struct rxrpc_call *);
void rxrpc_deactivate_bundle(struct rxrpc_bundle *bundle);
void rxrpc_discard_expired_client_conns(struct rxrpc_local *local);
void rxrpc_clean_up_local_conns(struct rxrpc_local *);

/*
 * conn_event.c
 */
void rxrpc_conn_retransmit_call(struct rxrpc_connection *conn, struct sk_buff *skb,
				unsigned int channel);
int rxrpc_abort_conn(struct rxrpc_connection *conn, struct sk_buff *skb,
		     s32 abort_code, int err, enum rxrpc_abort_reason why);
void rxrpc_process_connection(struct work_struct *);
void rxrpc_process_delayed_final_acks(struct rxrpc_connection *, bool);
bool rxrpc_input_conn_packet(struct rxrpc_connection *conn, struct sk_buff *skb);
void rxrpc_input_conn_event(struct rxrpc_connection *conn, struct sk_buff *skb);

static inline bool rxrpc_is_conn_aborted(const struct rxrpc_connection *conn)
{
	/* Order reading the abort info after the state check. */
	return smp_load_acquire(&conn->state) == RXRPC_CONN_ABORTED;
}

/*
 * conn_object.c
 */
extern unsigned int rxrpc_connection_expiry;
extern unsigned int rxrpc_closed_conn_expiry;

void rxrpc_poke_conn(struct rxrpc_connection *conn, enum rxrpc_conn_trace why);
struct rxrpc_connection *rxrpc_alloc_connection(struct rxrpc_net *, gfp_t);
struct rxrpc_connection *rxrpc_find_client_connection_rcu(struct rxrpc_local *,
							  struct sockaddr_rxrpc *,
							  struct sk_buff *);
void __rxrpc_disconnect_call(struct rxrpc_connection *, struct rxrpc_call *);
void rxrpc_disconnect_call(struct rxrpc_call *);
void rxrpc_kill_client_conn(struct rxrpc_connection *);
void rxrpc_queue_conn(struct rxrpc_connection *, enum rxrpc_conn_trace);
void rxrpc_see_connection(struct rxrpc_connection *, enum rxrpc_conn_trace);
struct rxrpc_connection *rxrpc_get_connection(struct rxrpc_connection *,
					      enum rxrpc_conn_trace);
struct rxrpc_connection *rxrpc_get_connection_maybe(struct rxrpc_connection *,
						    enum rxrpc_conn_trace);
void rxrpc_put_connection(struct rxrpc_connection *, enum rxrpc_conn_trace);
void rxrpc_service_connection_reaper(struct work_struct *);
void rxrpc_destroy_all_connections(struct rxrpc_net *);

static inline bool rxrpc_conn_is_client(const struct rxrpc_connection *conn)
{
	return conn->out_clientflag;
}

static inline bool rxrpc_conn_is_service(const struct rxrpc_connection *conn)
{
	return !rxrpc_conn_is_client(conn);
}

static inline void rxrpc_reduce_conn_timer(struct rxrpc_connection *conn,
					   unsigned long expire_at)
{
	timer_reduce(&conn->timer, expire_at);
}

/*
 * conn_service.c
 */
struct rxrpc_connection *rxrpc_find_service_conn_rcu(struct rxrpc_peer *,
						     struct sk_buff *);
struct rxrpc_connection *rxrpc_prealloc_service_connection(struct rxrpc_net *, gfp_t);
void rxrpc_new_incoming_connection(struct rxrpc_sock *, struct rxrpc_connection *,
				   const struct rxrpc_security *, struct sk_buff *);
void rxrpc_unpublish_service_conn(struct rxrpc_connection *);

/*
 * input.c
 */
void rxrpc_congestion_degrade(struct rxrpc_call *);
void rxrpc_input_call_packet(struct rxrpc_call *, struct sk_buff *);
void rxrpc_implicit_end_call(struct rxrpc_call *, struct sk_buff *);

/*
 * input_rack.c
 */
void rxrpc_input_rack_one(struct rxrpc_call *call,
			  struct rxrpc_ack_summary *summary,
			  struct rxrpc_txqueue *tq,
			  unsigned int ix);
void rxrpc_input_rack(struct rxrpc_call *call,
		      struct rxrpc_ack_summary *summary,
		      struct rxrpc_txqueue *tq,
		      unsigned long new_acks);
void rxrpc_rack_detect_loss_and_arm_timer(struct rxrpc_call *call,
					  struct rxrpc_ack_summary *summary);
ktime_t rxrpc_tlp_calc_pto(struct rxrpc_call *call, ktime_t now);
void rxrpc_tlp_send_probe(struct rxrpc_call *call);
void rxrpc_tlp_process_ack(struct rxrpc_call *call, struct rxrpc_ack_summary *summary);
void rxrpc_rack_timer_expired(struct rxrpc_call *call, ktime_t overran_by);

/* Initialise TLP state [RFC8958 7.1]. */
static inline void rxrpc_tlp_init(struct rxrpc_call *call)
{
	call->tlp_serial = 0;
	call->tlp_seq = call->acks_hard_ack;
	call->tlp_is_retrans = false;
}

/*
 * io_thread.c
 */
int rxrpc_encap_rcv(struct sock *, struct sk_buff *);
void rxrpc_error_report(struct sock *);
bool rxrpc_direct_abort(struct sk_buff *skb, enum rxrpc_abort_reason why,
			s32 abort_code, int err);
bool rxrpc_direct_conn_abort(struct sk_buff *skb, enum rxrpc_abort_reason why,
			     s32 abort_code, int err);
int rxrpc_io_thread(void *data);
void rxrpc_post_response(struct rxrpc_connection *conn, struct sk_buff *skb);
static inline void rxrpc_wake_up_io_thread(struct rxrpc_local *local)
{
	if (!local->io_thread)
		return;
	wake_up_process(READ_ONCE(local->io_thread));
}

static inline bool rxrpc_protocol_error(struct sk_buff *skb, enum rxrpc_abort_reason why)
{
	return rxrpc_direct_abort(skb, why, RX_PROTOCOL_ERROR, -EPROTO);
}

/*
 * insecure.c
 */
extern const struct rxrpc_security rxrpc_no_security;

/*
 * key.c
 */
extern struct key_type key_type_rxrpc;

int rxrpc_request_key(struct rxrpc_sock *, sockptr_t , int);
int rxrpc_get_server_data_key(struct rxrpc_connection *, const void *, time64_t,
			      u32);

/*
 * local_event.c
 */
void rxrpc_gen_version_string(void);
void rxrpc_send_version_request(struct rxrpc_local *local,
				struct rxrpc_host_header *hdr,
				struct sk_buff *skb);

/*
 * local_object.c
 */
void rxrpc_local_dont_fragment(const struct rxrpc_local *local, bool set);
struct rxrpc_local *rxrpc_lookup_local(struct net *, const struct sockaddr_rxrpc *);
struct rxrpc_local *rxrpc_get_local(struct rxrpc_local *, enum rxrpc_local_trace);
struct rxrpc_local *rxrpc_get_local_maybe(struct rxrpc_local *, enum rxrpc_local_trace);
void rxrpc_put_local(struct rxrpc_local *, enum rxrpc_local_trace);
struct rxrpc_local *rxrpc_use_local(struct rxrpc_local *, enum rxrpc_local_trace);
void rxrpc_unuse_local(struct rxrpc_local *, enum rxrpc_local_trace);
void rxrpc_destroy_local(struct rxrpc_local *local);
void rxrpc_destroy_all_locals(struct rxrpc_net *);

static inline bool __rxrpc_use_local(struct rxrpc_local *local,
				     enum rxrpc_local_trace why)
{
	int r, u;

	r = refcount_read(&local->ref);
	u = atomic_fetch_add_unless(&local->active_users, 1, 0);
	trace_rxrpc_local(local->debug_id, why, r, u);
	return u != 0;
}

static inline void rxrpc_see_local(struct rxrpc_local *local,
				   enum rxrpc_local_trace why)
{
	int r, u;

	r = refcount_read(&local->ref);
	u = atomic_read(&local->active_users);
	trace_rxrpc_local(local->debug_id, why, r, u);
}

/*
 * misc.c
 */
extern unsigned int rxrpc_max_backlog __read_mostly;
extern unsigned long rxrpc_soft_ack_delay;
extern unsigned long rxrpc_idle_ack_delay;
extern unsigned int rxrpc_rx_window_size;
extern unsigned int rxrpc_rx_mtu;
extern unsigned int rxrpc_rx_jumbo_max;
#ifdef CONFIG_AF_RXRPC_INJECT_RX_DELAY
extern unsigned long rxrpc_inject_rx_delay;
#endif

/*
 * net_ns.c
 */
extern unsigned int rxrpc_net_id;
extern struct pernet_operations rxrpc_net_ops;

static inline struct rxrpc_net *rxrpc_net(struct net *net)
{
	return net_generic(net, rxrpc_net_id);
}

/*
 * out_of_band.c
 */
void rxrpc_notify_socket_oob(struct rxrpc_call *call, struct sk_buff *skb);
void rxrpc_add_pending_oob(struct rxrpc_sock *rx, struct sk_buff *skb);
int rxrpc_sendmsg_oob(struct rxrpc_sock *rx, struct msghdr *msg, size_t len);

/*
 * output.c
 */
ssize_t do_udp_sendmsg(struct socket *socket, struct msghdr *msg, size_t len);
void rxrpc_send_ACK(struct rxrpc_call *call, u8 ack_reason,
		    rxrpc_serial_t serial, enum rxrpc_propose_ack_trace why);
void rxrpc_send_probe_for_pmtud(struct rxrpc_call *call);
int rxrpc_send_abort_packet(struct rxrpc_call *);
void rxrpc_send_data_packet(struct rxrpc_call *call, struct rxrpc_send_data_req *req);
void rxrpc_send_conn_abort(struct rxrpc_connection *conn);
void rxrpc_reject_packet(struct rxrpc_local *local, struct sk_buff *skb);
void rxrpc_send_keepalive(struct rxrpc_peer *);
void rxrpc_send_response(struct rxrpc_connection *conn, struct sk_buff *skb);

/*
 * peer_event.c
 */
void rxrpc_input_error(struct rxrpc_local *, struct sk_buff *);
void rxrpc_peer_keepalive_worker(struct work_struct *);
void rxrpc_input_probe_for_pmtud(struct rxrpc_connection *conn, rxrpc_serial_t acked_serial,
				 bool sendmsg_fail);

/*
 * peer_object.c
 */
struct rxrpc_peer *rxrpc_lookup_peer_rcu(struct rxrpc_local *,
					 const struct sockaddr_rxrpc *);
struct rxrpc_peer *rxrpc_lookup_peer(struct rxrpc_local *local,
				     struct sockaddr_rxrpc *srx, gfp_t gfp);
void rxrpc_assess_MTU_size(struct rxrpc_local *local, struct rxrpc_peer *peer);
struct rxrpc_peer *rxrpc_alloc_peer(struct rxrpc_local *, gfp_t,
				    enum rxrpc_peer_trace);
void rxrpc_new_incoming_peer(struct rxrpc_local *local, struct rxrpc_peer *peer);
void rxrpc_destroy_all_peers(struct rxrpc_net *);
struct rxrpc_peer *rxrpc_get_peer(struct rxrpc_peer *, enum rxrpc_peer_trace);
struct rxrpc_peer *rxrpc_get_peer_maybe(struct rxrpc_peer *, enum rxrpc_peer_trace);
void rxrpc_put_peer(struct rxrpc_peer *, enum rxrpc_peer_trace);

/*
 * proc.c
 */
extern const struct seq_operations rxrpc_call_seq_ops;
extern const struct seq_operations rxrpc_connection_seq_ops;
extern const struct seq_operations rxrpc_bundle_seq_ops;
extern const struct seq_operations rxrpc_peer_seq_ops;
extern const struct seq_operations rxrpc_local_seq_ops;

/*
 * recvmsg.c
 */
void rxrpc_notify_socket(struct rxrpc_call *);
int rxrpc_recvmsg(struct socket *, struct msghdr *, size_t, int);

/*
 * Abort a call due to a protocol error.
 */
static inline int rxrpc_abort_eproto(struct rxrpc_call *call,
				     struct sk_buff *skb,
				     s32 abort_code,
				     enum rxrpc_abort_reason why)
{
	struct rxrpc_skb_priv *sp = rxrpc_skb(skb);

	rxrpc_abort_call(call, sp->hdr.seq, abort_code, -EPROTO, why);
	return -EPROTO;
}

/*
 * rtt.c
 */
void rxrpc_call_add_rtt(struct rxrpc_call *call, enum rxrpc_rtt_rx_trace why,
			int rtt_slot,
			rxrpc_serial_t send_serial, rxrpc_serial_t resp_serial,
			ktime_t send_time, ktime_t resp_time);
ktime_t rxrpc_get_rto_backoff(struct rxrpc_call *call, bool retrans);
void rxrpc_call_init_rtt(struct rxrpc_call *call);

/*
 * rxgk.c
 */
extern const struct rxrpc_security rxgk_yfs;

/*
 * rxkad.c
 */
#ifdef CONFIG_RXKAD
extern const struct rxrpc_security rxkad;
#endif

/*
 * security.c
 */
int __init rxrpc_init_security(void);
const struct rxrpc_security *rxrpc_security_lookup(u8);
void rxrpc_exit_security(void);
int rxrpc_init_client_call_security(struct rxrpc_call *);
int rxrpc_init_client_conn_security(struct rxrpc_connection *);
const struct rxrpc_security *rxrpc_get_incoming_security(struct rxrpc_sock *,
							 struct sk_buff *);
struct key *rxrpc_look_up_server_security(struct rxrpc_connection *,
					  struct sk_buff *, u32, u32);

/*
 * sendmsg.c
 */
bool rxrpc_propose_abort(struct rxrpc_call *call, s32 abort_code, int error,
			 enum rxrpc_abort_reason why);
int rxrpc_do_sendmsg(struct rxrpc_sock *, struct msghdr *, size_t);

/*
 * server_key.c
 */
extern struct key_type key_type_rxrpc_s;

int rxrpc_server_keyring(struct rxrpc_sock *, sockptr_t, int);

/*
 * skbuff.c
 */
void rxrpc_kernel_data_consumed(struct rxrpc_call *, struct sk_buff *);
void rxrpc_new_skb(struct sk_buff *, enum rxrpc_skb_trace);
void rxrpc_see_skb(struct sk_buff *, enum rxrpc_skb_trace);
void rxrpc_eaten_skb(struct sk_buff *, enum rxrpc_skb_trace);
void rxrpc_get_skb(struct sk_buff *, enum rxrpc_skb_trace);
void rxrpc_free_skb(struct sk_buff *, enum rxrpc_skb_trace);
void rxrpc_purge_queue(struct sk_buff_head *);

/*
 * stats.c
 */
int rxrpc_stats_show(struct seq_file *seq, void *v);
int rxrpc_stats_clear(struct file *file, char *buf, size_t size);

#define rxrpc_inc_stat(rxnet, s) atomic_inc(&(rxnet)->s)
#define rxrpc_dec_stat(rxnet, s) atomic_dec(&(rxnet)->s)

/*
 * sysctl.c
 */
#ifdef CONFIG_SYSCTL
extern int __init rxrpc_sysctl_init(void);
extern void rxrpc_sysctl_exit(void);
#else
static inline int __init rxrpc_sysctl_init(void) { return 0; }
static inline void rxrpc_sysctl_exit(void) {}
#endif

/*
 * txbuf.c
 */
extern atomic_t rxrpc_nr_txbuf;
struct rxrpc_txbuf *rxrpc_alloc_data_txbuf(struct rxrpc_call *call, size_t data_size,
					   size_t data_align, gfp_t gfp);
void rxrpc_see_txbuf(struct rxrpc_txbuf *txb, enum rxrpc_txbuf_trace what);
void rxrpc_put_txbuf(struct rxrpc_txbuf *txb, enum rxrpc_txbuf_trace what);

/*
 * utils.c
 */
int rxrpc_extract_addr_from_skb(struct sockaddr_rxrpc *, struct sk_buff *);

static inline bool before(u32 seq1, u32 seq2)
{
        return (s32)(seq1 - seq2) < 0;
}
static inline bool before_eq(u32 seq1, u32 seq2)
{
        return (s32)(seq1 - seq2) <= 0;
}
static inline bool after(u32 seq1, u32 seq2)
{
        return (s32)(seq1 - seq2) > 0;
}
static inline bool after_eq(u32 seq1, u32 seq2)
{
        return (s32)(seq1 - seq2) >= 0;
}

static inline u32 earliest(u32 seq1, u32 seq2)
{
	return before(seq1, seq2) ? seq1 : seq2;
}

static inline u32 latest(u32 seq1, u32 seq2)
{
	return after(seq1, seq2) ? seq1 : seq2;
}

static inline bool rxrpc_seq_in_txq(const struct rxrpc_txqueue *tq, rxrpc_seq_t seq)
{
	return (seq & (RXRPC_NR_TXQUEUE - 1)) == tq->qbase;
}

static inline void rxrpc_queue_rx_call_packet(struct rxrpc_call *call, struct sk_buff *skb)
{
	rxrpc_get_skb(skb, rxrpc_skb_get_call_rx);
	__skb_queue_tail(&call->rx_queue, skb);
	rxrpc_poke_call(call, rxrpc_call_poke_rx_packet);
}

/*
 * Calculate how much space there is for transmitting more DATA packets.
 */
static inline unsigned int rxrpc_tx_window_space(const struct rxrpc_call *call)
{
	int winsize = umin(call->tx_winsize, call->cong_cwnd + call->cong_extra);
	int transmitted = call->tx_top - call->tx_bottom;

	return max(winsize - transmitted, 0);
}

static inline unsigned int rxrpc_left_out(const struct rxrpc_call *call)
{
	return call->acks_nr_sacks + call->tx_nr_lost;
}

/*
 * Calculate the number of transmitted DATA packets assumed to be in flight
 * [approx RFC6675].
 */
static inline unsigned int rxrpc_tx_in_flight(const struct rxrpc_call *call)
{
	return call->tx_nr_sent - rxrpc_left_out(call) + call->tx_nr_resent;
}

/*
 * debug tracing
 */
extern unsigned int rxrpc_debug;

#define dbgprintk(FMT,...) \
	printk("[%-6.6s] "FMT"\n", current->comm ,##__VA_ARGS__)

#define kenter(FMT,...)	dbgprintk("==> %s("FMT")",__func__ ,##__VA_ARGS__)
#define kleave(FMT,...)	dbgprintk("<== %s()"FMT"",__func__ ,##__VA_ARGS__)
#define kdebug(FMT,...)	dbgprintk("    "FMT ,##__VA_ARGS__)


#if defined(__KDEBUG)
#define _enter(FMT,...)	kenter(FMT,##__VA_ARGS__)
#define _leave(FMT,...)	kleave(FMT,##__VA_ARGS__)
#define _debug(FMT,...)	kdebug(FMT,##__VA_ARGS__)

#elif defined(CONFIG_AF_RXRPC_DEBUG)
#define RXRPC_DEBUG_KENTER	0x01
#define RXRPC_DEBUG_KLEAVE	0x02
#define RXRPC_DEBUG_KDEBUG	0x04

#define _enter(FMT,...)					\
do {							\
	if (unlikely(rxrpc_debug & RXRPC_DEBUG_KENTER))	\
		kenter(FMT,##__VA_ARGS__);		\
} while (0)

#define _leave(FMT,...)					\
do {							\
	if (unlikely(rxrpc_debug & RXRPC_DEBUG_KLEAVE))	\
		kleave(FMT,##__VA_ARGS__);		\
} while (0)

#define _debug(FMT,...)					\
do {							\
	if (unlikely(rxrpc_debug & RXRPC_DEBUG_KDEBUG))	\
		kdebug(FMT,##__VA_ARGS__);		\
} while (0)

#else
#define _enter(FMT,...)	no_printk("==> %s("FMT")",__func__ ,##__VA_ARGS__)
#define _leave(FMT,...)	no_printk("<== %s()"FMT"",__func__ ,##__VA_ARGS__)
#define _debug(FMT,...)	no_printk("    "FMT ,##__VA_ARGS__)
#endif

/*
 * debug assertion checking
 */
#if 1 // defined(__KDEBUGALL)

#define ASSERT(X)						\
do {								\
	if (unlikely(!(X))) {					\
		pr_err("Assertion failed\n");			\
		BUG();						\
	}							\
} while (0)

#define ASSERTCMP(X, OP, Y)						\
do {									\
	__typeof__(X) _x = (X);						\
	__typeof__(Y) _y = (__typeof__(X))(Y);				\
	if (unlikely(!(_x OP _y))) {					\
		pr_err("Assertion failed - %lu(0x%lx) %s %lu(0x%lx) is false\n", \
		       (unsigned long)_x, (unsigned long)_x, #OP,	\
		       (unsigned long)_y, (unsigned long)_y);		\
		BUG();							\
	}								\
} while (0)

#define ASSERTIF(C, X)						\
do {								\
	if (unlikely((C) && !(X))) {				\
		pr_err("Assertion failed\n");			\
		BUG();						\
	}							\
} while (0)

#define ASSERTIFCMP(C, X, OP, Y)					\
do {									\
	__typeof__(X) _x = (X);						\
	__typeof__(Y) _y = (__typeof__(X))(Y);				\
	if (unlikely((C) && !(_x OP _y))) {				\
		pr_err("Assertion failed - %lu(0x%lx) %s %lu(0x%lx) is false\n", \
		       (unsigned long)_x, (unsigned long)_x, #OP,	\
		       (unsigned long)_y, (unsigned long)_y);		\
		BUG();							\
	}								\
} while (0)

#else

#define ASSERT(X)				\
do {						\
} while (0)

#define ASSERTCMP(X, OP, Y)			\
do {						\
} while (0)

#define ASSERTIF(C, X)				\
do {						\
} while (0)

#define ASSERTIFCMP(C, X, OP, Y)		\
do {						\
} while (0)

#endif /* __KDEBUGALL */
